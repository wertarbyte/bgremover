#include "background_remover.h"

#include "glog/logging.h"

#ifdef WITH_GL
#include "tensorflow/lite/delegates/gpu/delegate.h"
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <execution>
#include <numeric>
#include <vector>

const char *deeplabv3_label_names[] = {
    "background", "aeroplane", "bicycle",     "bird",  "board",       "bottle", "bus",
    "car",        "cat",       "chair",       "cow",   "diningtable", "dog",    "horse",
    "motorbike",  "person",    "pottedplant", "sheep", "sofa",        "train",  "tv",
};

constexpr int deeplabv3_label_count =
    sizeof(deeplabv3_label_names) / sizeof(deeplabv3_label_names[0]);
typedef float DeeplabV3Labels[deeplabv3_label_count];

static std::string tensor_shape(const TfLiteTensor *t) {
    std::stringstream ret;
    ret << "[";
    for (int i = 0; i < TfLiteTensorNumDims(t); i++) {
        if (i > 0) ret << ", ";
        ret << TfLiteTensorDim(t, i);
    }
    ret << "]";
    return ret.str();
}

BackgroundRemover::ModelType BackgroundRemover::parseModelType(const std::string &model_type) {
    if (model_type == "deeplabv3")
        return BackgroundRemover::ModelType::DeeplabV3;
    else if (model_type == "bodypix_resnet")
        return BackgroundRemover::ModelType::BodypixResnet;
    else if (model_type == "bodypix_mobilenet")
        return BackgroundRemover::ModelType::BodypixMobilenet;
    else
        return BackgroundRemover::ModelType::Undefined;
}

BackgroundRemover::BackgroundRemover(const std::string &model_filename,
                                     const std::string &model_type, int num_threads)
    : model_type_(parseModelType(model_type)) {
    static_assert(sizeof(float) == 4, "floats must be 32 bits");

    CHECK(model_type_ != ModelType::Undefined) << "Invalid model type " << model_type;

    model_ = CHECK_NOTNULL(TfLiteModelCreateFromFile(model_filename.c_str()));

    options_ = CHECK_NOTNULL(TfLiteInterpreterOptionsCreate());

    TfLiteInterpreterOptionsSetNumThreads(options_, num_threads);
    TfLiteInterpreterOptionsSetErrorReporter(
        options_,
        [](void *unused, const char *fmt, va_list args) {
            std::vector<char> buf(vsnprintf(nullptr, 0, fmt, args) + 1);
            std::vsnprintf(buf.data(), buf.size(), fmt, args);
            LOG(ERROR) << "Tensorflow: " << buf.data();
        },
        nullptr);
#ifdef WITH_GL
    auto delegate_opts = TfLiteGpuDelegateOptionsV2Default();
    delegate_opts.inference_preference = TFLITE_GPU_INFERENCE_PREFERENCE_SUSTAINED_SPEED;
    delegate_opts.inference_priority1 = TFLITE_GPU_INFERENCE_PRIORITY_MIN_LATENCY;
    gpu_delegate_ = CHECK_NOTNULL(TfLiteGpuDelegateV2Create(&delegate_opts));
    TfLiteInterpreterOptionsAddDelegate(options_, gpu_delegate_);
#endif

    interpreter_ = CHECK_NOTNULL(TfLiteInterpreterCreate(model_, options_));
    TfLiteInterpreterAllocateTensors(interpreter_);

    input_ = CHECK_NOTNULL(TfLiteInterpreterGetInputTensor(interpreter_, 0));
    LOG(INFO) << "Input tensor: " << tensor_shape(input_);
    CHECK_EQ(TfLiteTensorType(input_), kTfLiteFloat32) << "input tensor must be float32";
    CHECK_EQ(TfLiteTensorNumDims(input_), 4) << "input tensor must have 4 dimensions";
    CHECK_EQ(TfLiteTensorDim(input_, 0), 1) << "input tensor batch size must be 1";
    width_ = TfLiteTensorDim(input_, 1);
    height_ = TfLiteTensorDim(input_, 2);
    CHECK_EQ(TfLiteTensorDim(input_, 3), 3) << "input tensor must have 3 channels";

    output_ = CHECK_NOTNULL(TfLiteInterpreterGetOutputTensor(interpreter_, 0));
    LOG(INFO) << "Output tensor: " << tensor_shape(output_);
    CHECK_EQ(TfLiteTensorType(output_), kTfLiteFloat32) << "output tensor must be float32";
    CHECK_EQ(TfLiteTensorNumDims(output_), 4) << "output tensor must have 4 dimensions";
    int outw = TfLiteTensorDim(output_, 1);
    CHECK_EQ(width_ % outw, 0) << "output tensor width is not a multiple of input tensor width";
    stride_ = width_ / outw;
    int outh = TfLiteTensorDim(output_, 2);
    CHECK_EQ(height_ % outh, 0) << "output tensor height is not a multiple of input tensor height";
    CHECK_EQ(height_ / outh, stride_) << "vertical stride doesn't match horizontal stride";

    if (model_type_ == ModelType::DeeplabV3) {
        CHECK_EQ(stride_, 1);
        CHECK_EQ(TfLiteTensorDim(output_, 3), deeplabv3_label_count);
    } else if (model_type_ == ModelType::BodypixResnet) {
        CHECK(stride_ == 16 || stride_ == 32);
        CHECK_EQ(TfLiteTensorDim(output_, 3), 1);
    } else if (model_type_ == ModelType::BodypixMobilenet) {
        CHECK(stride_ == 8 || stride_ == 16);
        CHECK_EQ(TfLiteTensorDim(output_, 3), 1);
    }

    LOG(INFO) << "Initialized tflite with " << width_ << "x" << height_
              << "px input and stride=" << stride_ << " for model " << model_filename;
}

static float minVec3f(const cv::Vec3f &v) { return std::min({v[0], v[1], v[2]}); }

static float maxVec3f(const cv::Vec3f &v) { return std::max({v[0], v[1], v[2]}); }

static void checkValuesInRange(const cv::Mat &mat, float min, float max) {
#ifndef NDEBUG
    const auto [matmin, matmax] = std::minmax_element(
        std::execution::par_unseq, mat.begin<cv::Vec3f>(), mat.end<cv::Vec3f>(),
        [](const cv::Vec3f &a, const cv::Vec3f &b) { return minVec3f(a) < minVec3f(b); });
    CHECK_GE(minVec3f(*matmin), min);
    CHECK_LE(maxVec3f(*matmax), max);
#endif
}

cv::Mat BackgroundRemover::makeInputTensor(const cv::Mat &img) {
    cv::Mat ret;
    switch (model_type_) {
        case ModelType::DeeplabV3:
        case ModelType::BodypixMobilenet:
            img.convertTo(ret, CV_32FC3, 1. / 255, -.5);
            checkValuesInRange(ret, -.5, .5);
            break;

        case ModelType::BodypixResnet:
            img.convertTo(ret, CV_32FC3);
            // https://github.com/tensorflow/tfjs-models/blob/master/body-pix/src/resnet.ts#L22
            std::for_each(std::execution::par_unseq, ret.begin<cv::Vec3f>(), ret.end<cv::Vec3f>(),
                          [](cv::Vec3f &v) { v += cv::Vec3f(-123.15, -115.90, -103.06); });
            checkValuesInRange(ret, -127., 255.);  // ?
            break;

        default:
            CHECK(0);
    }

    return ret;
}

cv::Mat BackgroundRemover::getMaskFromOutput() {
    constexpr int person_label = 15;  // XXX
    constexpr float threshold = .5;   // XXX

    int maskw = width_ / stride_;
    int maskh = height_ / stride_;

    cv::Mat ret = cv::Mat::zeros(cv::Size(maskw, maskh), CV_8U);

    size_t size = TfLiteTensorByteSize(output_);
    void *data = TfLiteTensorData(output_);

    if (model_type_ == ModelType::DeeplabV3) {
        CHECK_EQ(size, maskw * maskh * sizeof(DeeplabV3Labels));
        DeeplabV3Labels *labels = (DeeplabV3Labels *)data;
        std::for_each(std::execution::par_unseq, labels, labels + maskw * maskh,
                      [&](DeeplabV3Labels l) {
                          float *max = std::max_element(l, l + deeplabv3_label_count);
                          int label = max - l;
                          if (label != person_label) {
                              int pixel = (DeeplabV3Labels *)l - labels;
                              ret.at<unsigned char>(cv::Point(pixel % maskw, pixel / maskw)) = 1;
                          }
                      });
    } else {
        CHECK_EQ(size, maskw * maskh * sizeof(float));
        float *prob = (float *)data;
        std::for_each(std::execution::par_unseq, prob, prob + maskw * maskh, [&](float &p) {
            if (p < threshold) {
                int pixel = &p - prob;
                ret.at<unsigned char>(cv::Point(pixel % maskw, pixel / maskw)) = 1;
            }
        });
    }

    return ret;
}

void BackgroundRemover::maskBackground(cv::Mat &frame /* rgb */,
                                       const cv::Mat &maskImage /* rgb */) {
    CHECK_EQ(frame.size, maskImage.size);
    cv::Mat small;
    cv::resize(frame, small, cv::Size(width_, height_), interpolation_method);

    cv::Mat input_float = makeInputTensor(small);
    CHECK_EQ(input_float.elemSize(), sizeof(float) * 3 /* channels */);

    TfLiteTensorCopyFromBuffer(input_, (const void *)input_float.ptr<float>(),
                               width_ * height_ * sizeof(float) * 3);

    auto start = std::chrono::steady_clock::now();
    TfLiteInterpreterInvoke(interpreter_);
    auto end = std::chrono::steady_clock::now();
    auto diffMs = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    LOG(INFO) << "Inference time: " << diffMs << "ms";

    cv::Mat mask = getMaskFromOutput();
    cv::resize(mask, mask, cv::Size(frame.cols, frame.rows), interpolation_method);

    // XXX: Fix this.
    for (int x = 0; x < frame.cols; x++)
        for (int y = 0; y < frame.rows; y++)
            if (mask.at<unsigned char>(cv::Point(x, y)))
                frame.at<cv::Vec3b>(cv::Point(x, y)) = maskImage.at<cv::Vec3b>(cv::Point(x, y));
    // frame.setTo(maskImage, mask);
    // cv::bitwise_and(frame, mask, frame);
}

BackgroundRemover::~BackgroundRemover() {
    TfLiteInterpreterDelete(interpreter_);
#ifdef WITH_GL
    TfLiteGpuDelegateV2Delete(gpu_delegate_);
#endif
    TfLiteInterpreterOptionsDelete(options_);
    TfLiteModelDelete(model_);
}
