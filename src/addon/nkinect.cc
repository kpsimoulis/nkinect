#include <nan.h>
#include <iostream>
#include <unistd.h>
#include <limits>

extern "C" {
  #include <libfreenect/libfreenect.h>
}

typedef enum {
        NKinectFrameModeDepth, NKinectFrameModeVideo
} NKinectFrameMode;

typedef struct {
  uint device = 0;
  bool autoInit = true;
  int delay = 0;
  int maxTiltAngle = 31;
  int minTiltAngle = -31;
  freenect_loglevel logLevel = (freenect_loglevel)(FREENECT_LOG_DEBUG);
  freenect_device_flags capabilities = (freenect_device_flags)(FREENECT_DEVICE_MOTOR | FREENECT_DEVICE_CAMERA);
} NKinectInitOptions;


class NKinect : public Nan::ObjectWrap {
public:
bool running = false;
bool sending = false;
freenect_device*       device;
freenect_context*      context;
freenect_raw_tilt_state* state;
freenect_frame_mode videoMode;
freenect_frame_mode depthMode;
uv_async_t uv_async_video_callback;
uv_async_t uv_async_depth_callback;
uint8_t *videoBuffer;
uint8_t *depthBuffer;
uv_loop_t *loop = uv_default_loop();
uv_thread_t event_thread;
Nan::Callback *callback_video;
Nan::Callback *callback_depth;
NKinectInitOptions options;
explicit NKinect(NKinectInitOptions opts) {

        this->options = opts;
        if (freenect_init(&this->context, NULL) < 0) {
                Nan::ThrowError("Error initializing freenect context");
                return;
        }
        freenect_set_log_level(this->context, this->options.logLevel);
        freenect_select_subdevices(this->context, this->options.capabilities);
        int nr_devices = freenect_num_devices(this->context);
        if (nr_devices < 1) {
                this->Close();
                Nan::ThrowError("No kinect devices present");
                return;
        }

        if (freenect_open_device(this->context, &this->device, this->options.device) < 0) {
                this->Close();
                Nan::ThrowError("Could not open device number\n");
                return;
        }

        freenect_set_user(this->device, this);
        this->state = freenect_get_tilt_state(this->device);

        if(this->options.autoInit){
              this->Resume();
        }

}
~NKinect() {
        this->Close();
}

void DepthCallback(){
        this->sending = true;
        const unsigned argc = 1;
        v8::Isolate * isolate = v8::Isolate::GetCurrent();
        v8::HandleScope handleScope(isolate);
        Nan::MaybeLocal<v8::Object> buffer = Nan::CopyBuffer((char*)this->depthBuffer, this->depthMode.bytes);
        v8::Local<v8::Value> argv[argc] = { buffer.ToLocalChecked() };
        this->callback_depth->Call(argc, argv);
        this->sending = false;
}

void StartDepthCapture(const v8::Local<v8::Function> &callback) {
        return this->StartDepthCapture(callback, Nan::New<v8::Object>());
}

void StartDepthCapture(const v8::Local<v8::Function> &callback, const v8::Local<v8::Object> &options) {
        // Check if previous video capture running settings are the same and ignore stop and initialization
        this->StopDepthCapture();
        this->callback_depth = new Nan::Callback(callback);
        this->depthMode = NKinect::freenect_find_mode_by_options(NKinectFrameModeDepth, options);
        if(!this->depthMode.is_valid) {
                Nan::ThrowError("Invalid depth configuration\n");
                return;
        }

        if (freenect_set_depth_mode(this->device, this->depthMode) != 0) {
                Nan::ThrowError("Error setting depth mode\n");
                return;
        };

        freenect_set_depth_callback(this->device, NKinect::freenect_device_depth_cb);

        this->depthBuffer = (uint8_t*)malloc(this->depthMode.bytes);

        if (freenect_set_depth_buffer(this->device, this->depthBuffer) != 0) {
                Nan::ThrowError("Error setting depth buffer\n");
                return;
        };

        if (freenect_start_depth(this->device) != 0) {
                Nan::ThrowError("Error starting depth\n");
                return;
        }

        uv_async_init(this->loop, &this->uv_async_depth_callback, NKinect::async_depth_callback);
}

void StopDepthCapture(){
        freenect_stop_depth(this->device);
}

void VideoCallback(){
        this->sending = true;
        const unsigned argc = 1;
        v8::Isolate * isolate = v8::Isolate::GetCurrent();
        v8::HandleScope handleScope(isolate);
        Nan::MaybeLocal<v8::Object> buffer = Nan::CopyBuffer((char*)this->videoBuffer, this->videoMode.bytes);
        v8::Local<v8::Value> argv[argc] = { buffer.ToLocalChecked() };
        this->callback_video->Call(argc, argv);
        this->sending = false;
}

void StartVideoCapture(const v8::Local<v8::Function> &callback) {
        return this->StartVideoCapture(callback, Nan::New<v8::Object>());
}

void StartVideoCapture(const v8::Local<v8::Function> &callback, const v8::Local<v8::Object> &options) {
        // Check if previous video capture running settings are the same and ignore stop and initialization
        this->StopVideoCapture();
        this->callback_video = new Nan::Callback(callback);
        this->videoMode = NKinect::freenect_find_mode_by_options(NKinectFrameModeVideo, options);
        // this->videoMode = freenect_find_video_mode(FREENECT_RESOLUTION_MEDIUM, FREENECT_VIDEO_RGB);
        if(!this->videoMode.is_valid) {
                Nan::ThrowError("Invalid video configuration\n");
                return;
        }

        if (freenect_set_video_mode(this->device, this->videoMode) != 0) {
                Nan::ThrowError("Error setting video mode\n");
                return;
        };

        freenect_set_video_callback(this->device, NKinect::freenect_device_video_cb);

        this->videoBuffer = (uint8_t*)malloc(this->videoMode.bytes);

        if (freenect_set_video_buffer(this->device, this->videoBuffer) != 0) {
                Nan::ThrowError("Error setting video buffer\n");
                return;
        };

        if (freenect_start_video(this->device) != 0) {
                Nan::ThrowError("Error starting video\n");
                return;
        }

        uv_async_init(this->loop, &this->uv_async_video_callback, NKinect::async_video_callback);
}

void StopVideoCapture(){
        freenect_stop_video(this->device);
}

void SetLedState(freenect_led_options state){
      if(freenect_set_led(this->device, state) < 0){
          Nan::ThrowError("Error setting led state");
      };

}

double GetLedState() {
         return 0;
}

void SetTiltAngle(const double angle) {
        if(freenect_set_tilt_degs(this->device,
              std::min<double>(this->options.maxTiltAngle,
              std::max<double>(this->options.minTiltAngle, angle))) < 0){
              Nan::ThrowError("Error setting tilt angle");
        }

}

double GetTiltAngle() {
         return freenect_get_tilt_degs(this->state);
}

freenect_tilt_status_code
GetTiltStatus() {
         return freenect_get_tilt_status(this->state);
}

void Resume(){
        if (!this->running) {
                this->running = true;
                if(uv_thread_create(&this->event_thread, NKinect::pthread_callback, (void*)this) != 0) {
                        Nan::ThrowError("Error creating thread\n");
                        return;
                }
        }
}

void Pause(){
        if (this->running) {
                this->running = false;
                uv_thread_join(&this->event_thread);
        }
}

void Close(){
        this->running = false;

        if (this->device != NULL) {
                if (freenect_close_device(this->device) < 0) {
                        Nan::ThrowError("Error closing device");
                        return;
                }

                this->device = NULL;
        }

        if (this->context != NULL) {
                if (freenect_shutdown(this->context) < 0) {
                        Nan::ThrowError("Error shutting down");
                        return;
                }

                this->context = NULL;
        }
}

void ProcessEventsLoop(){
        freenect_raw_tilt_state * state;
        int8_t tilt_angle;
        while(this->running) {
            tilt_angle = this->state->tilt_angle;
            // state = (freenect_raw_tilt_state * )this->state;
            // printf("%d\n", this->state->tilt_angle);
            // memcpy(&this->state, &state,sizeof(freenect_raw_tilt_state));
            if(this->options.delay != 0){
                static timeval timeout = { 0, this->options.delay };
                freenect_process_events_timeout(this->context, &timeout);
            } else {
                freenect_process_events(this->context);
            }

            // printf("%d \n", /*state.tilt_angle,*/ this->state->tilt_angle);
            // printf("%d \n", /*state.tilt_angle,*/ this->GetTiltAngle());
//            printf("%d <-> %d\n", tilt_angle,  this->state->tilt_angle);
        }
}

static
void
pthread_callback(void *user_data) {
        NKinect* kinect = static_cast<NKinect*>(user_data);
        kinect->ProcessEventsLoop();
}

static
void
async_video_callback(uv_async_t *handle) {
        NKinect* context = static_cast<NKinect*>(handle->data);
        context->VideoCallback();
}

static
void
async_depth_callback(uv_async_t *handle) {
        NKinect* context = static_cast<NKinect*>(handle->data);
        context->DepthCallback();
}

static
void
freenect_device_video_cb(freenect_device *dev, void *video, uint32_t timestamp)
{
        NKinect* context = static_cast<NKinect*>(freenect_get_user(dev));
        if (context->sending) return;
        context->uv_async_video_callback.data = (void *) context;
        uv_async_send(&context->uv_async_video_callback);
}

static
void
freenect_device_depth_cb(freenect_device *dev, void *depth, uint32_t timestamp)
{
        NKinect* context = static_cast<NKinect*>(freenect_get_user(dev));
        if (context->sending) return;
        context->uv_async_depth_callback.data = (void *) context;
        uv_async_send(&context->uv_async_depth_callback);
}

static
freenect_frame_mode
freenect_find_mode_by_options(NKinectFrameMode mode, const v8::Local<v8::Object> &options){
        v8::Local<v8::Value> fmt = options->Get(Nan::New<v8::String>("format").ToLocalChecked());
        v8::Local<v8::Value> res = options->Get(Nan::New<v8::String>("resolution").ToLocalChecked());

        if(!fmt->IsNumber())
                res = Nan::New<v8::Number>(FREENECT_RESOLUTION_MEDIUM);

        if(mode == NKinectFrameModeDepth){
            if(!fmt->IsNumber())
                    res = Nan::New<v8::Number>(FREENECT_DEPTH_11BIT);
            return freenect_find_depth_mode(static_cast<freenect_resolution>(res->Uint32Value()), static_cast<freenect_depth_format>(fmt->Uint32Value()));
        }

        if(!fmt->IsNumber())
                res = Nan::New<v8::Number>(FREENECT_VIDEO_RGB);
        return freenect_find_video_mode(static_cast<freenect_resolution>(res->Uint32Value()), static_cast<freenect_video_format>(fmt->Uint32Value()));
}

static
NKinectInitOptions
freenect_init_options(const v8::Local<v8::Object> &options){
    v8::Local<v8::Value> device = options->Get(Nan::New<v8::String>("device").ToLocalChecked());
    v8::Local<v8::Value> autoinit = options->Get(Nan::New<v8::String>("auto").ToLocalChecked());
    v8::Local<v8::Value> maxTiltAngle = options->Get(Nan::New<v8::String>("maxTiltAngle").ToLocalChecked());
    v8::Local<v8::Value> minTiltAngle = options->Get(Nan::New<v8::String>("minTiltAngle").ToLocalChecked());
    v8::Local<v8::Value> logLevel = options->Get(Nan::New<v8::String>("logLevel").ToLocalChecked());
    v8::Local<v8::Value> capabilities = options->Get(Nan::New<v8::String>("capabilities").ToLocalChecked());

    // bool autoInit = true;
    // int maxTiltAngle = 31;
    // int minTiltAngle = -31;
    // freenect_loglevel loglevel = (freenect_device_flags)(FREENECT_LOG_DEBUG);
    // freenect_device_flags capabilities = (freenect_device_flags)(FREENECT_DEVICE_MOTOR | FREENECT_DEVICE_CAMERA);

    NKinectInitOptions opts;

    if(device->IsNumber())
            opts.device = device->NumberValue();
    if(autoinit->IsBoolean())
            opts.autoInit = autoinit->IsTrue();
    if(maxTiltAngle->IsNumber())
            opts.maxTiltAngle = maxTiltAngle->NumberValue();
    if(minTiltAngle->IsNumber())
            opts.minTiltAngle = minTiltAngle->NumberValue();
    if(logLevel->IsNumber())
            opts.logLevel = (freenect_loglevel)(logLevel->NumberValue());
    if(capabilities->IsNumber())
            opts.capabilities = (freenect_device_flags)(capabilities->NumberValue());

    return opts;
}

static NAN_MODULE_INIT(Init) {
        v8::Local<v8::FunctionTemplate> tpl = Nan::New<v8::FunctionTemplate>(New);
        tpl->SetClassName(Nan::New("NKinectDevice").ToLocalChecked());
        tpl->InstanceTemplate()->SetInternalFieldCount(1);

        Nan::SetAccessor(tpl->InstanceTemplate(), Nan::New<v8::String>("running").ToLocalChecked(), getRunning);
        Nan::SetAccessor(tpl->InstanceTemplate(), Nan::New<v8::String>("sending").ToLocalChecked(), getSending);

        Nan::SetPrototypeMethod(tpl, "setTiltAngle", NKinect::setTiltAngle);
        Nan::SetPrototypeMethod(tpl, "getTiltAngle", NKinect::GetTiltAngle);
        Nan::SetPrototypeMethod(tpl, "setLedState", NKinect::SetLedState);
        Nan::SetPrototypeMethod(tpl, "getLedState", NKinect::GetLedState);
        Nan::SetPrototypeMethod(tpl, "startVideo", NKinect::StartVideo);
        Nan::SetPrototypeMethod(tpl, "stopVideo", NKinect::StopVideo);
        Nan::SetPrototypeMethod(tpl, "startDepth", NKinect::StartDepth);
        Nan::SetPrototypeMethod(tpl, "stopDepth", NKinect::StopDepth);
        Nan::SetPrototypeMethod(tpl, "resume", NKinect::Resume);
        Nan::SetPrototypeMethod(tpl, "pause", NKinect::Pause);

        constructor().Reset(Nan::GetFunction(tpl).ToLocalChecked());
        Nan::Set(target, Nan::New("NKinect").ToLocalChecked(),
                 Nan::GetFunction(tpl).ToLocalChecked());
}
private:
static NAN_METHOD(New) {
        if (info.IsConstructCall()) {
                NKinectInitOptions opts;
                if(info[0]->IsObject())
                    opts = freenect_init_options(info[0].As<v8::Object>());

                printf("autoInit %d, device %d, maxTiltAngle %d, minTiltAngle %d, logLevel %d, capabilities %d\n",
                opts.autoInit,
                opts.device,
                opts.maxTiltAngle,
                opts.minTiltAngle,
                opts.logLevel,
                opts.capabilities);

                NKinect *obj = new NKinect(opts);
                obj->Wrap(info.This());
                info.GetReturnValue().Set(info.This());
        } else {
                const int argc = 1;
                v8::Local<v8::Value> argv[argc] = {info[0]};
                v8::Local<v8::Function> cons = Nan::New(constructor());
                info.GetReturnValue().Set(Nan::NewInstance(cons, argc, argv).ToLocalChecked());
        }
}

static NAN_GETTER(getRunning) {
        NKinect* obj = Nan::ObjectWrap::Unwrap<NKinect>(info.Holder());
        info.GetReturnValue().Set(Nan::New<v8::Boolean>(obj->running));
}

static NAN_GETTER(getSending) {
        NKinect* obj = Nan::ObjectWrap::Unwrap<NKinect>(info.Holder());
        info.GetReturnValue().Set(Nan::New<v8::Boolean>(obj->sending));
}

static NAN_METHOD(StartVideo) {
        if (info.Length() < 1)
                return Nan::ThrowError("Expecting at least one argument in StartVideo");
        NKinect* obj = Nan::ObjectWrap::Unwrap<NKinect>(info.Holder());
        if (info.Length() == 1) {
                if(!info[0]->IsFunction())
                        return Nan::ThrowError("Calback argument must be a function in StartVideo");
                obj->StartVideoCapture(info[0].As<v8::Function>());
        } else {
                if(!info[0]->IsObject())
                        return Nan::ThrowError("Options argument must be a object in StartVideo");
                if(!info[1]->IsFunction())
                        return Nan::ThrowError("Calback argument must be a function in StartVideo");
                obj->StartVideoCapture(info[1].As<v8::Function>(), info[0].As<v8::Object>());
        }

        info.GetReturnValue().Set(obj->handle());
}

static NAN_METHOD(StartDepth) {
        if (info.Length() < 1)
                return Nan::ThrowError("Expecting at least one argument in StartDepth");
        NKinect* obj = Nan::ObjectWrap::Unwrap<NKinect>(info.Holder());
        if (info.Length() == 1) {
                if(!info[0]->IsFunction())
                        return Nan::ThrowError("Calback argument must be a function in StartDepth");
                obj->StartDepthCapture(info[0].As<v8::Function>());
        } else {
                if(!info[0]->IsObject())
                        return Nan::ThrowError("Options argument must be a object in StartDepth");
                if(!info[1]->IsFunction())
                        return Nan::ThrowError("Calback argument must be a function in StartDepth");
                obj->StartDepthCapture(info[1].As<v8::Function>(), info[0].As<v8::Object>());
        }

        info.GetReturnValue().Set(obj->handle());
}

static NAN_METHOD(Resume) {
        NKinect* obj = Nan::ObjectWrap::Unwrap<NKinect>(info.Holder());
        obj->Resume();
        info.GetReturnValue().Set(obj->handle());
}

static NAN_METHOD(Pause) {
        NKinect* obj = Nan::ObjectWrap::Unwrap<NKinect>(info.Holder());
        obj->Pause();
        info.GetReturnValue().Set(obj->handle());
}

static NAN_METHOD(StopVideo) {
        NKinect* obj = Nan::ObjectWrap::Unwrap<NKinect>(info.Holder());
        obj->StopVideoCapture();
        info.GetReturnValue().Set(obj->handle());
}

static NAN_METHOD(StopDepth) {
        NKinect* obj = Nan::ObjectWrap::Unwrap<NKinect>(info.Holder());
        obj->StopDepthCapture();
        info.GetReturnValue().Set(obj->handle());
}

static NAN_METHOD(GetTiltAngle) {
        NKinect* obj = Nan::ObjectWrap::Unwrap<NKinect>(info.Holder());
        info.GetReturnValue().Set(obj->GetTiltAngle());
}

static NAN_METHOD(setTiltAngle) {
        if (info.Length() == 1) {
                if (!info[0]->IsNumber())
                        return Nan::ThrowError("Tilt argument must be a number");
        } else {
                return Nan::ThrowError("Expecting at least one argument with the tilt angle");
        }

        double angle = info[0]->NumberValue();
        NKinect* obj = Nan::ObjectWrap::Unwrap<NKinect>(info.Holder());
        obj->SetTiltAngle(angle);
        info.GetReturnValue().Set(obj->handle());
}

static NAN_METHOD(SetLedState) {
        if (info.Length() == 1) {
                if (!info[0]->IsNumber())
                        return Nan::ThrowError("Led State argument must be a number");
        } else {
                return Nan::ThrowError("Expecting at least one argument with the led state");
        }

        freenect_led_options state = (freenect_led_options)(info[0]->NumberValue());
        NKinect* obj = Nan::ObjectWrap::Unwrap<NKinect>(info.Holder());
        obj->SetLedState(state);
        info.GetReturnValue().Set(obj->handle());
}

static NAN_METHOD(GetLedState) {
        NKinect* obj = Nan::ObjectWrap::Unwrap<NKinect>(info.Holder());
        info.GetReturnValue().Set(obj->GetLedState());
}

static inline Nan::Persistent<v8::Function> & constructor() {
        static Nan::Persistent<v8::Function> freenect_constructor;
        return freenect_constructor;
}
};

NODE_MODULE(objectwrapper, NKinect::Init)
