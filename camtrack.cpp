#include <cassert>
#include <chrono>
#include <cmath>
#include <iostream>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <err.h>
#include <fcntl.h>
#include <unistd.h>

#include <opencv2/core/cuda.hpp>
#include <opencv2/cudaimgproc.hpp>
#include <opencv2/cudaobjdetect.hpp>
#include <opencv2/cudawarping.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

using std::cout;
using std::cerr;
using std::endl;

typedef float prec;
constexpr int kOperScale = 4;
constexpr int kOutWidth = 640;
constexpr int kOutHeight = 480;
constexpr prec kOutAspect = (prec)kOutWidth / kOutHeight;
// These rates are initialized in main.  They're more or less used as
// constants, but there are knobs to adjust them.
prec kLPFSlewRate;
prec kBoundedMaxSlew;
prec kBoundedMaxSlewAccel;
prec kZoom;
prec kEyes;

inline void
dbgRect(cv::Mat &img __attribute__((unused)),
        cv::Rect rect __attribute__((unused)),
        const cv::Scalar &color __attribute__((unused)),
        int thickness __attribute__((unused)) = 1,
        int lineType __attribute__((unused)) = cv::LINE_8,
        int shift __attribute__((unused)) = 0) {
  cv::rectangle(img,
                cv::Rect(rect.x/kOperScale,
                         rect.y/kOperScale,
                         rect.width/kOperScale,
                         rect.height/kOperScale),
                color, thickness, lineType, shift);
}

// ABC
template<typename T=prec>
class SmoothMoverABC {
 public:
  explicit virtual operator T() const = 0;
  virtual void operator<<(T target) = 0;
};

// Simple low-pass filter.
template<typename T=prec>
class SmoothMoverLPF : public SmoothMoverABC<T> {
 public:
  explicit SmoothMoverLPF(T initial) : s_(initial) {};
  SmoothMoverLPF(const SmoothMoverLPF&) = default;
  SmoothMoverLPF& operator=(const SmoothMoverLPF&) = default;
  SmoothMoverLPF(SmoothMoverLPF&&) = default;
  SmoothMoverLPF& operator=(SmoothMoverLPF&&) = default;
  explicit operator T() const override { return (T)s_; }
  void operator<<(T target) override {
    s_ = s_ * (1 - kLPFSlewRate) + target * kLPFSlewRate;
  }

 private:
  T s_;
};

// A constant acceleration, and bounded velocity.  (The acceleration
// can be lower than the constant at the end of the slew.)
template<typename T=prec>
class SmoothMoverBoundedAccel : public SmoothMoverABC<T> {
 public:
  explicit SmoothMoverBoundedAccel(T initial) : s_(initial) {};
  SmoothMoverBoundedAccel(const SmoothMoverBoundedAccel&) = default;
  SmoothMoverBoundedAccel& operator=(const SmoothMoverBoundedAccel&) = default;
  SmoothMoverBoundedAccel(SmoothMoverBoundedAccel&&) = default;
  SmoothMoverBoundedAccel& operator=(SmoothMoverBoundedAccel&&) = default;
  explicit operator T() const override { return s_; }
  void operator<<(T target) override;

 private:
  T s_;
  T ds_;
};

template<typename T> void
SmoothMoverBoundedAccel<T>::operator<<(T target) {
  if (target < s_) {
    ds_ -= kBoundedMaxSlewAccel;
    if (ds_ < -kBoundedMaxSlew)
      ds_ = -kBoundedMaxSlew;
    if (s_ + ds_ < target)
      ds_ = target - s_;
    s_ += ds_;
    assert(s_ >= target);
  } else if (target > s_) {
    ds_ += kBoundedMaxSlewAccel;
    if (ds_ > kBoundedMaxSlew)
      ds_ = kBoundedMaxSlew;
    if (s_ + ds_ > target)
      ds_ = s_ - target;
    s_ += ds_;
    assert(s_ <= target);
  }
}

template<typename R, typename S, typename T=prec>
class SmoothMoverCompose : public SmoothMoverABC<T> {
 public:
  explicit SmoothMoverCompose(T initial) : r_(initial), s_(initial) {};
  SmoothMoverCompose(const SmoothMoverCompose&) = default;
  SmoothMoverCompose& operator=(const SmoothMoverCompose&) = default;
  SmoothMoverCompose(SmoothMoverCompose&&) = default;
  SmoothMoverCompose& operator=(SmoothMoverCompose&&) = default;
  explicit operator T() const override { return static_cast<T>(s_); }
  void operator<<(T target) override {
    r_ << target;
    s_ << static_cast<T>(r_);
  }

 private:
  R r_;
  S s_;
};

// XXX After fiddling with this some, I think I came to an interesting
// conclusion.  First, perception is better tuned to seeing scaling
// effects than panning effects.  Second, it's probably better tuned
// to pixel-scale jumps than non-pixel-scale jumps.  Third, the
// "obvious" algorithms -- everything I can invent without lots of
// work -- tend to build cutoffs are divisible by 4, and hence often
// make pixel-scale jumps.  I think I should experiment within the
// x:y:width:height framework (that cv::Rect does) instead of trying
// to use x0:y0:x1:y1 like I do here.
template<typename T=prec>
class SmoothMovingRect {
 public:
  template<typename Q>
  SmoothMovingRect(cv::Rect_<Q> bounds, T aspect)
      : bounds_(bounds),
        aspect_(aspect),
        centerX_(bounds.x + static_cast<T>(bounds.width) / 2),
        centerY_(bounds.y + static_cast<T>(bounds.height) / 2),
        size_(bounds.width * bounds.height) {};
  SmoothMovingRect(const SmoothMovingRect&) = default;
  SmoothMovingRect& operator=(const SmoothMovingRect&) = default;
  SmoothMovingRect(SmoothMovingRect&&) = default;
  SmoothMovingRect& operator=(SmoothMovingRect&&) = default;

  template<typename Q>
  cv::Rect_<Q> scale(T factor, T verticalPositioning) const;
  template<typename Q> explicit operator cv::Rect_<Q>() const {
    return scale<Q>(1, 0.5);
  }
  void operator<<(const cv::Rect_<T>&);

 private:
  const cv::Rect_<T> bounds_;
  const T aspect_;
  SmoothMoverCompose<SmoothMoverBoundedAccel<T>,
                     SmoothMoverLPF<T>,
                     T>
  centerX_, centerY_;
  SmoothMoverLPF<T> size_;
};

template<typename T> template<typename Q>
cv::Rect_<Q>
SmoothMovingRect<T>::scale(T factor, T verticalPositioning) const {
  T size = static_cast<T>(size_) * factor;
  T aspect = aspect_;
  T height = std::sqrt(size / aspect);
  T width = aspect * height;
  T x = static_cast<T>(centerX_) - (width / 2);
  T y = static_cast<T>(centerY_) - (height * verticalPositioning);

#if 0
  cout << width << "x"
       << height << "+"
       << x << "+"
       << y << " / "
       << bounds_.width << "x"
       << bounds_.height << "+"
       << bounds_.x << "+"
       << bounds_.y;
#endif

  // Shrink it into bounds, while maintaining the aspect ratio.  We
  // always shrink from all edges, to maintain the center.
  if (x < 0) {
    T delta = -x;
    width -= 2 * delta;
    height -= 2 * delta / aspect;
    x = 0;
  }
  if (y < 0) {
    T delta = -y;
    height -= 2 * delta;
    width -= 2 * delta * aspect;
    y = 0;
  }
  T right = x + width;
  T bounds_right = bounds_.x + bounds_.width;
  if (right > bounds_right) {
    T delta = right - bounds_right;
    width -= 2 * delta;
    height -= 2 * delta / aspect;
    x += delta;
    y += delta / aspect;
  }
  T bottom = y + height;
  T bounds_bottom = bounds_.y + bounds_.height;
  if (bottom > bounds_bottom) {
    T delta = bottom - bounds_bottom;
    height -= 2 * delta;
    width -= 2 * delta * aspect;
    y += delta;
    x += delta * aspect;
  }

#if 0
  cout << " -> "
       << width << "x"
       << height << "+"
       << x << "+"
       << y << endl;
#endif

  return cv::Rect_<Q>(x, y, width, height);
}

template<typename T> void
SmoothMovingRect<T>::operator<<(const cv::Rect_<T>& target) {
  centerX_ << target.x + target.width / 2;
  centerY_ << target.y + target.height / 2;
  size_ << target.width * target.height;
#if 0
  cout << static_cast<T>(centerX_) << ","
       << static_cast<T>(centerY_) << " @ "
       << static_cast<T>(size_) << endl;
#endif
}

template<typename T> cv::Rect_<T>
rectBound(const std::vector<cv::Rect_<T>> &rects)
{
  assert(!rects.empty());
  cv::Rect_<T> rv = rects[0];
  for (size_t i = 1; i < rects.size(); i++) {
    rv |= rects[i];
  }
  return rv;
}

int
main()
{
  cout << "CUDA devices: " << cv::cuda::getCudaEnabledDeviceCount() << endl;
  cv::cuda::setDevice(0);
  cv::cuda::printShortCudaDeviceInfo(0);

  // alt2: just spins the GPU
  // alt_tree: doesn't recognize me
  // alt: doesn't see me often
  // default: lots of false positives
  auto face_cascade = cv::cuda::CascadeClassifier::create(
      "haarcascade_cuda.xml");
  // The CPU and GPU defaults are listed here.
  face_cascade->setScaleFactor(1.1);                  // 1.1, 1.2
  face_cascade->setMinNeighbors(3);                   // 3, 4

  int out_fd = open("/dev/video0", O_RDWR);
  if (out_fd < 0)
    err(1, "/dev/video0");
  struct v4l2_format vid_format;
  memset(&vid_format, 0, sizeof(vid_format));
  vid_format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  if (ioctl(out_fd, VIDIOC_G_FMT, &vid_format) < 0)
    err(1, "VIDIOC_G_FMT");
  vid_format.fmt.pix.width = kOutWidth;
  vid_format.fmt.pix.height = kOutHeight;
  // Chrome only supports:
  // V4L2_PIX_FMT_YUV420, V4L2_PIX_FMT_Y16, V4L2_PIX_FMT_Z16,
  // V4L2_PIX_FMT_INVZ, V4L2_PIX_FMT_YUYV, V4L2_PIX_FMT_RGB24,
  // V4L2_PIX_FMT_MJPEG, V4L2_PIX_FMT_JPEG
  // Discord doesn't work with RGB24, but does with YUV420.
  vid_format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
  vid_format.fmt.pix.sizeimage = kOutHeight * kOutWidth * 3 / 2;
  vid_format.fmt.pix.field = V4L2_FIELD_NONE;
  if (ioctl(out_fd, VIDIOC_S_FMT, &vid_format) < 0)
    err(1, "VIDIOC_S_FMT");

  // We use cv::VideoCapture instead of cv::cudacodec::VideoReader
  // because my camera outputs its high-resolution images in 4:2:2.
  // cv::cudacodec::VideoReader only works with 4:2:0 images.
  // You can check a file's format with
  // identify -format "%[jpeg:sampling-factor]\n" foo.jpg
  //     4:4:4: 1x1,1x1,1x1
  //     4:2:2: 2x1,1x1,1x1
  //     4:2:0: 2x2,1x1,1x1
  // I don't know why cv::cudacodec::VideoReader requires 4:2:0; it says
  // it needs it to render to an NV12 surface.  But according to my
  // experiment, my card (at least) can render YUV422 to NV12, or at
  // least says it can.  While NV12 is essentially a reordering of I420,
  // it can just throw away half the chroma to get it from I422.
  cv::VideoCapture cam(2, cv::CAP_V4L2);
  if (!cam.isOpened())
    errx(1, "open camera 2");
  cam.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
  // At 1440x1080 and below, I can get 19fps after all the processing.
  // But at 1920x1080, it drops to 14fps.
  cam.set(cv::CAP_PROP_FRAME_WIDTH, 1440);
  cam.set(cv::CAP_PROP_FRAME_HEIGHT, 1080);
  int camWidth = cam.get(cv::CAP_PROP_FRAME_WIDTH);
  int camHeight = cam.get(cv::CAP_PROP_FRAME_HEIGHT);
  cv::Size outSize(kOutWidth, kOutHeight);
  cout << cam.getBackendName() << " frame size "
       << camWidth << "x" << camHeight << endl;
  uint32_t fourcc = cam.get(cv::CAP_PROP_FOURCC);
  cout << "fourcc: "
       << static_cast<char>((fourcc >>  0) & 0x7f)
       << static_cast<char>((fourcc >>  8) & 0x7f)
       << static_cast<char>((fourcc >> 16) & 0x7f)
       << static_cast<char>((fourcc >> 24) & 0x7f)
       << endl;

  std::string opwin("CamTrack Operator");
  cv::namedWindow(opwin, cv::WINDOW_OPENGL);
  cv::resizeWindow(opwin, camWidth/4, camHeight/4 + 200);
  std::string outwin("CamTrack Output");
  cv::namedWindow(outwin, cv::WINDOW_OPENGL);
  cv::resizeWindow(outwin, kOutWidth, kOutHeight);

  int minimumSizeDivisor = 10;
  int classifierPyrCount = 0;
  auto adjustCascadeSizes =
      [camWidth, camHeight, &classifierPyrCount, face_cascade,
       &minimumSizeDivisor] {
        int minSize = (camWidth >> classifierPyrCount) / minimumSizeDivisor;
        cout << minSize << endl;
        face_cascade->setMinObjectSize(cv::Size(minSize, minSize));
        face_cascade->setMaxObjectSize(cv::Size(minSize*10, minSize*10));
      };
  adjustCascadeSizes();
  std::function<void(void)> acsFunc = adjustCascadeSizes;
  auto acsCallback = [](int, void* acsVoid) {
                       auto acsFunc =
                           static_cast<std::function<void(void)>*>(acsVoid);
                       (*acsFunc)();
                     };
  cv::createTrackbar("Minimum size divisor", opwin, &minimumSizeDivisor, 64,
                     acsCallback, &acsFunc);
  cv::createTrackbar("Classifier prescale", opwin, &classifierPyrCount, 4,
                     acsCallback, &acsFunc);
  int initial = 20;
  cv::createTrackbar("Slew LPF weight", opwin, &initial, 50,
                     [](int pos, void*) {
                       kLPFSlewRate = exp10(pos / -10.0);
                     });
  kLPFSlewRate = exp10(initial / -10.0);
  initial = 10;
  cv::createTrackbar("Slew vel", opwin, &initial, 20,
                     [](int pos, void*) {
                       kBoundedMaxSlew = pos / 10.0;
                     });
  kBoundedMaxSlew = initial / 10.0;
  initial = 75;
  cv::createTrackbar("Slew accel", opwin, &initial, 200,
                     [](int pos, void*) {
                       kBoundedMaxSlewAccel = pos / 1000.0;
                     });
  kBoundedMaxSlew = initial / 1000.0;
  initial = 60;
  cv::createTrackbar("Zoom", opwin, &initial, 150,
                     [](int pos, void*) {
                       kZoom = (pos+1) / 10.0;
                     });
  kZoom = initial / 10.0;
  initial = 40;
  cv::createTrackbar("Eyes", opwin, &initial, 120,
                     [](int pos, void*) {
                       kEyes = pos / 120.0;
                     });
  kEyes = initial / 120.0;

  cv::cuda::Stream background;
  cv::cuda::Stream operator_stream;

  cv::Mat input_cpu;
  cv::cuda::GpuMat input;
  cv::cuda::GpuMat input_gray;
  cv::Mat operator_display;
  cv::cuda::GpuMat operator_display_gpu;
  cv::cuda::GpuMat faces_gpu;
  std::vector<cv::Rect> faces_cpu;
  cv::cuda::GpuMat output;
  cv::Mat output_cpu;

  SmoothMovingRect<prec> roi(cv::Rect(0, 0, camWidth, camHeight), kOutAspect);

  int interval_frames = 0;
  auto interval_start = std::chrono::steady_clock::now();

  while (1) {
    cam >> input_cpu;

    input.upload(input_cpu, background);
    cv::cuda::cvtColor(input, input_gray, cv::COLOR_BGR2GRAY, 0, background);
    for (int i = 0; i < classifierPyrCount; i++)
      cv::cuda::pyrDown(input_gray, input_gray, background);
    cv::cuda::equalizeHist(input_gray, input_gray, background);
    // The CUDA cascade classifier has a stream argument, but doesn't
    // work with it (it asserts out).  We'll build our operator display
    // while the rest is running, then start the classifier in the
    // foreground.
    cv::resize(input_cpu, operator_display, cv::Size(),
               1.0/kOperScale, 1.0/kOperScale, cv::INTER_NEAREST);
    background.waitForCompletion();
    face_cascade->detectMultiScale(input_gray, faces_gpu);
    face_cascade->convert(faces_gpu, faces_cpu);

    cv::Rect faceBounds;
    if (faces_cpu.size() > 0) {
      cv::Rect faceBoundsUnscaled = rectBound(faces_cpu);
      faceBounds = cv::Rect(faceBoundsUnscaled.x << classifierPyrCount,
                            faceBoundsUnscaled.y << classifierPyrCount,
                            faceBoundsUnscaled.width << classifierPyrCount,
                            faceBoundsUnscaled.height << classifierPyrCount);
      roi << faceBounds;
    }
    auto xmit = roi.scale<prec>(kZoom, kEyes);

    //cout << xmit << endl;

    try {
      cv::Point2f src[]{
                       {xmit.x, xmit.y},
                       {xmit.x, xmit.y + xmit.height},
                       {xmit.x + xmit.width, xmit.y + xmit.height}};
      cv::Point2f dst[]{
                       {0, 0},
                       {0, kOutHeight},
                       {kOutWidth, kOutHeight}};
      auto xfrm = cv::getAffineTransform(src, dst);
      cv::cuda::warpAffine(input, output, xfrm,
                           cv::Size{kOutWidth, kOutHeight},
                           cv::INTER_LINEAR,
                           cv::BORDER_CONSTANT,
                           cv::Scalar(),
                           background);
    } catch (cv::Exception &e) {
      // Debugging aid
      cerr << "Output " << xmit << endl;
      throw e;
    }

    if (operator_stream.queryIfComplete()) {
      if (!operator_display_gpu.empty())
        cv::imshow(opwin, operator_display_gpu);
      for (auto&& face : faces_cpu) {
        cv::Rect scaledFace(face.x << classifierPyrCount,
                            face.y << classifierPyrCount,
                            face.width << classifierPyrCount,
                            face.height << classifierPyrCount);
        dbgRect(operator_display, scaledFace, cv::Scalar(0, 255, 0));
      }
      dbgRect(operator_display, static_cast<cv::Rect>(roi),
              cv::Scalar(255, 0, 0));
      dbgRect(operator_display, xmit, cv::Scalar(38, 38, 238));
      operator_display_gpu.upload(operator_display, operator_stream);
    }

    background.waitForCompletion();  // Wait for the affine transform
    output.download(output_cpu, background);
    cv::imshow(outwin, output);
    // cv::cuda::cvtColor only supports YUV at 4:4:4, and V4L2 doesn't
    // have that as a planar format.
    //cv::cuda::cvtColor(output, output, cv::COLOR_BGR2YUV, 0, background);
    background.waitForCompletion();
    cv::cvtColor(output_cpu, output_cpu, cv::COLOR_BGR2YUV_I420);
    auto written = write(out_fd, output_cpu.data,
                         kOutHeight * kOutWidth * 3 / 2);
    if (written < 0)
      err(1, "write frame");

    cv::waitKey(10);

    interval_frames++;
    auto now = std::chrono::steady_clock::now();
    std::chrono::duration<double> interval_duration = now - interval_start;
    if (interval_duration.count() >= 1) {
      int fps = interval_frames / interval_duration.count();
      cout << "FPS: " << fps << endl;
      cout << "ROI: " << static_cast<cv::Rect>(roi) << endl;
      interval_frames = 0;
      interval_start = now;
    }
  }

  cout << "End of stream" << endl;
}

// Local Variables:
// compile-command: "g++ -Werror -Wall -Wextra -Og -g -I/usr/include/opencv4 camtrack.cpp -lopencv_core -lopencv_cudacodec -lopencv_cudaimgproc -lopencv_cudaobjdetect -lopencv_cudawarping -lopencv_highgui -lopencv_imgproc -o camtrack"
// End:
