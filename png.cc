#include <node.h>
#include <node_buffer.h>
#include <png.h>
#include <cstdlib>
#include <vector>
#include <utility>
#include <string>
#include <cassert>

using namespace v8;
using namespace node;

static Handle<Value>
VException(const char *msg) {
    HandleScope scope;
    return ThrowException(Exception::Error(String::New(msg)));
}

static inline bool
str_eq(const char *s1, const char *s2)
{
    return strcmp(s1, s2) == 0;
}

typedef enum { BUF_RGB, BUF_BGR, BUF_RGBA, BUF_BGRA } buffer_type;

class PngEncoder {
private:
    int width, height;
    unsigned char *data;
    char *png;
    int png_len, mem_len;
    buffer_type buf_type; 

    std::string encoder_error;

public: 
    static void
    png_chunk_producer(png_structp png_ptr, png_bytep data, png_size_t length)
    {
        PngEncoder *p = (PngEncoder *)png_get_io_ptr(png_ptr);
        if (!p->encoder_error.empty()) return;

        if (!p->png) {
            p->png = (char *)malloc(sizeof(p->png)*41);
            if (!p->png) {
                p->encoder_error = "malloc failed in node-png (PngEncoder::png_chunk_producer)";
                return;
            }
            p->mem_len = 41;
        }

        if (p->png_len + length > p->mem_len) {
            char *new_png = (char *)realloc(p->png, sizeof(char)*p->png_len + length);
            if (!new_png) {
                free(p->png);
                p->encoder_error = "realloc failed in node-png (PngEncoder::png_chunk_producer).";
                return;
            }
            p->png = new_png;
            p->mem_len += length;
        }
        memcpy(p->png + p->png_len, data, length);
        p->png_len += length;
    }

    PngEncoder(unsigned char *ddata, int wwidth, int hheight, buffer_type bbuf_type) :
        data(ddata), width(wwidth), height(hheight), buf_type(bbuf_type),
        png(NULL), png_len(0), mem_len(0) {}

    ~PngEncoder() {
        free(png);
    }

    Handle<Value>
    encode()
    {
        HandleScope scope;

        png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
        if (!png_ptr)
            return VException("png_create_write_struct failed.");

        png_infop info_ptr = png_create_info_struct(png_ptr);
        if (!png_ptr)
            return VException("png_create_info_struct failed.");

        int color_type;
        if (buf_type == BUF_RGB || buf_type == BUF_BGR)
            color_type = PNG_COLOR_TYPE_RGB;
        else
            color_type = PNG_COLOR_TYPE_RGB_ALPHA;

        png_set_IHDR(png_ptr, info_ptr, width, height,
                 8, color_type, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);

        png_set_write_fn(png_ptr, (void *)this, png_chunk_producer, NULL);

        png_write_info(png_ptr, info_ptr);
        png_set_invert_alpha(png_ptr);

        if (buf_type == BUF_BGR || buf_type == BUF_BGRA)
            png_set_bgr(png_ptr);

        png_bytep *row_pointers = (png_bytep *)malloc(sizeof(png_bytep) * height);
        if (!row_pointers)
            return VException("malloc failed in node-png (PngEncoder::encode).");

        if (buf_type == BUF_RGB || buf_type == BUF_BGR) {
            for (int i=0; i<height; i++)
                row_pointers[i] = data+3*i*width;
        }
        else {
            for (int i=0; i<height; i++)
                row_pointers[i] = data+4*i*width;
        }

        png_write_image(png_ptr, row_pointers);
        png_write_end(png_ptr, NULL);
        png_destroy_write_struct(&png_ptr, &info_ptr);
        free(row_pointers);

        if (!encoder_error.empty())
            return VException(encoder_error.c_str());

        return Undefined();
    }

    const char *get_png() const {
        return png;
    }

    int get_png_len() const {
        return png_len;
    }
};

class Png : public ObjectWrap {
private:
    int width;
    int height;
    Buffer *data;
    buffer_type buf_type;

public:
    static void
    Initialize(Handle<Object> target)
    {
        HandleScope scope;

        Local<FunctionTemplate> t = FunctionTemplate::New(New);
        t->InstanceTemplate()->SetInternalFieldCount(1);
        NODE_SET_PROTOTYPE_METHOD(t, "encode", PngEncode);
        target->Set(String::NewSymbol("Png"), t->GetFunction());
    }

    Png(Buffer *ddata, int wwidth, int hheight, buffer_type bbuf_type) :
        data(ddata), width(wwidth), height(hheight), buf_type(bbuf_type) {}

    Handle<Value>
    PngEncode()
    {
        HandleScope scope;

        PngEncoder p((unsigned char *)data->data(), width, height, buf_type);
        Handle<Value> ret = p.encode();
        if (!ret->IsUndefined()) return ret;
        return scope.Close(Encode((char *)p.get_png(), p.get_png_len(), BINARY));
    }

protected:
    static Handle<Value>
    New(const Arguments &args)
    {
        HandleScope scope;

        if (args.Length() < 3)
            return VException("At least three arguments required - data buffer, width, height, [and input buffer type]");
        if (!Buffer::HasInstance(args[0]))
            return VException("First argument must be Buffer.");
        if (!args[1]->IsInt32())
            return VException("Second argument must be integer width.");
        if (!args[2]->IsInt32())
            return VException("Third argument must be integer height.");

        buffer_type buf_type = BUF_RGB;
        if (args.Length() == 4) {
            if (!args[3]->IsString())
                return VException("Fourth argument must be 'rgb', 'bgr', 'rgba' or 'bgra'.");

            String::AsciiValue bts(args[3]->ToString());
            if (!(str_eq(*bts, "rgb") || str_eq(*bts, "bgr") ||
                str_eq(*bts, "rgba") || str_eq(*bts, "bgra")))
            {
                return VException("Fourth argument must be 'rgb', 'bgr', 'rgba' or 'bgra'.");
            }
            
            if (str_eq(*bts, "rgb"))
                buf_type = BUF_RGB;
            else if (str_eq(*bts, "bgr"))
                buf_type = BUF_BGR;
            else if (str_eq(*bts, "rgba"))
                buf_type = BUF_RGBA;
            else if (str_eq(*bts, "bgra"))
                buf_type = BUF_BGRA;
            else
                return VException("Fourth argument wasn't 'rgb', 'bgr', 'rgba' or 'bgra'.");
        }

        Buffer *data = ObjectWrap::Unwrap<Buffer>(args[0]->ToObject());
        int w = args[1]->Int32Value();
        int h = args[2]->Int32Value();

        if (w < 0)
            return VException("Width smaller than 0.");
        if (h < 0)
            return VException("Height smaller than 0.");

        Png *png = new Png(data, w, h, buf_type);
        png->Wrap(args.This());
        return args.This();
    }

    static Handle<Value>
    PngEncode(const Arguments &args)
    {
        HandleScope scope;
        Png *png = ObjectWrap::Unwrap<Png>(args.This());
        return png->PngEncode();
    }
};

class FixedPngStack : public ObjectWrap {
private:
    int width, height;
    unsigned char *data;
    buffer_type buf_type;

public:
    static void
    Initialize(Handle<Object> target)
    {
        HandleScope scope;

        Local<FunctionTemplate> t = FunctionTemplate::New(New);
        t->InstanceTemplate()->SetInternalFieldCount(1);
        NODE_SET_PROTOTYPE_METHOD(t, "push", Push);
        NODE_SET_PROTOTYPE_METHOD(t, "encode", PngEncode);
        target->Set(String::NewSymbol("FixedPngStack"), t->GetFunction());
    }

    FixedPngStack(int wwidth, int hheight, buffer_type bbuf_type) :
        width(wwidth), height(hheight), buf_type(bbuf_type)
    { 
        data = (unsigned char *)malloc(sizeof(*data) * width * height * 4);
        if (!data) throw "malloc failed in node-png (FixedPngStack ctor)";
        memset(data, 0xFF, width*height*4);
    }

    ~FixedPngStack() { free(data); }

    void Push(Buffer *buf, int x, int y, int w, int h) {
        unsigned char *buf_data = (unsigned char *)buf->data();
        int start = y*width*4 + x*4;
        if (buf_type == BUF_RGB || buf_type == BUF_BGR) {
            for (int i = 0; i < h; i++) {
                for (int j = 0, k = 0; k < 3*w; j+=4, k+=3) {
                    data[start + i*width*4 + j] = buf_data[i*w*3 + k];
                    data[start + i*width*4 + j + 1] = buf_data[i*w*3 + k + 1];
                    data[start + i*width*4 + j + 2] = buf_data[i*w*3 + k + 2];
                    data[start + i*width*4 + j + 3] = 0x00;
                }
            }
        }
        else {
            for (int i = 0; i < h; i++) {
                for (int j = 0; j < 4*w; j+=4) {
                    data[start + i*width*4 + j] = buf_data[i*w*4 + j];
                    data[start + i*width*4 + j + 1] = buf_data[i*w*4 + j + 1];
                    data[start + i*width*4 + j + 2] = buf_data[i*w*4 + j + 2];
                    data[start + i*width*4 + j + 3] = buf_data[i*w*4 + j + 3];
                }
            }
        }
    }

    Handle<Value> PngEncode() {
        HandleScope scope;

        buffer_type pbt = BUF_RGBA;
        if (buf_type == BUF_BGR || buf_type == BUF_BGRA)
            pbt = BUF_BGRA;

        PngEncoder p(data, width, height, pbt);
        Handle<Value> ret = p.encode();
        if (!ret->IsUndefined()) return ret;
        return scope.Close(Encode((char *)p.get_png(), p.get_png_len(), BINARY));
    }

protected:
    static Handle<Value>
    New(const Arguments &args)
    {
        HandleScope scope;

        if (args.Length() < 2)
            return VException("At least two arguments required - width and height [and input buffer type].");
        if (!args[0]->IsInt32())
            return VException("First argument must be integer width.");
        if (!args[1]->IsInt32())
            return VException("Second argument must be integer height.");

        buffer_type buf_type = BUF_RGB;
        if (args.Length() == 3) {
            if (!args[2]->IsString())
                return VException("Third argument must be 'rgb', 'bgr', 'rgba' or 'bgra'.");

            String::AsciiValue bts(args[2]->ToString());
            if (!(str_eq(*bts, "rgb") || str_eq(*bts, "bgr") ||
                str_eq(*bts, "rgba") || str_eq(*bts, "bgra")))
            {
                return VException("Third argument must be 'rgb', 'bgr', 'rgba' or 'bgra'.");
            }
            
            if (str_eq(*bts, "rgb"))
                buf_type = BUF_RGB;
            else if (str_eq(*bts, "bgr"))
                buf_type = BUF_BGR;
            else if (str_eq(*bts, "rgba"))
                buf_type = BUF_RGBA;
            else if (str_eq(*bts, "bgra"))
                buf_type = BUF_BGRA;
            else
                return VException("Third argument wasn't 'rgb', 'bgr', 'rgba' or 'bgra'.");
        }

        try {
            FixedPngStack *png_stack = new FixedPngStack(
                args[0]->Int32Value(), args[1]->Int32Value(), buf_type
            );
            png_stack->Wrap(args.This());
            return args.This();
        }
        catch (const char *e) {
            return VException(e);
        }
    }

    static Handle<Value>
    Push(const Arguments &args)
    {
        HandleScope scope;

        if (!Buffer::HasInstance(args[0]))
            return VException("First argument must be Buffer.");
        if (!args[1]->IsInt32())
            return VException("Second argument must be integer x.");
        if (!args[2]->IsInt32())
            return VException("Third argument must be integer y.");
        if (!args[3]->IsInt32())
            return VException("Fourth argument must be integer w.");
        if (!args[4]->IsInt32())
            return VException("Fifth argument must be integer h.");

        FixedPngStack *png_stack = ObjectWrap::Unwrap<FixedPngStack>(args.This());
        Buffer *data_buf = ObjectWrap::Unwrap<Buffer>(args[0]->ToObject());
        int x = args[1]->Int32Value();
        int y = args[2]->Int32Value();
        int w = args[3]->Int32Value();
        int h = args[4]->Int32Value();

        if (x < 0)
            return VException("Coordinate x smaller than 0.");
        if (y < 0)
            return VException("Coordinate y smaller than 0.");
        if (w < 0)
            return VException("Width smaller than 0.");
        if (h < 0)
            return VException("Height smaller than 0.");
        if (x >= png_stack->width) 
            return VException("Coordinate x exceeds FixedPngStack's dimensions.");
        if (y >= png_stack->height) 
            return VException("Coordinate y exceeds FixedPngStack's dimensions.");
        if (x+w > png_stack->width) 
            return VException("Pushed PNG exceeds FixedPngStack's width.");
        if (y+h > png_stack->height) 
            return VException("Pushed PNG exceeds FixedPngStack's height.");

        png_stack->Push(data_buf, x, y, w, h);

        return Undefined();
    }

    static Handle<Value>
    PngEncode(const Arguments &args)
    {
        HandleScope scope;

        FixedPngStack *png_stack = ObjectWrap::Unwrap<FixedPngStack>(args.This());
        return png_stack->PngEncode();
    }
};

class DynamicPngStack : public ObjectWrap {
private:
    struct Png {
        int len, x, y, w, h;
        unsigned char *data;

        Png(unsigned char *ddata, int llen, int xx, int yy, int ww, int hh) :
            len(llen), x(xx), y(yy), w(ww), h(hh)
        {
            data = (unsigned char *)malloc(sizeof(*data)*len);
            if (!data) throw "malloc failed in DynamicPngStack::Png::Png";
            memcpy(data, ddata, len);
        }

        ~Png() {
            free(data);
        }
    };

    struct Point {
        int x, y;
        Point() {}
        Point(int xx, int yy) : x(xx), y(yy) {}
    };
    
    typedef std::vector<Png *> vPng;
    typedef vPng::iterator vPngi;
    vPng png_stack;
    Point offset;
    int width, height;
    buffer_type buf_type;

    std::pair<Point, Point> OptimalDimension() {
        Point top(-1, -1), bottom(-1, -1);
        for (vPngi it = png_stack.begin(); it != png_stack.end(); ++it) {
            Png *png = *it;
            if (top.x == -1 || png->x < top.x)
                top.x = png->x;
            if (top.y == -1 || png->y < top.y)
                top.y = png->y;
            if (bottom.x == -1 || png->x + png->w > bottom.x)
                bottom.x = png->x + png->w;
            if (bottom.y == -1 || png->y + png->h > bottom.y)
                bottom.y = png->y + png->h;
        }

        /*
        printf("top    x, y: %d, %d\n", top.x, top.y);
        printf("bottom x, y: %d, %d\n", bottom.x, bottom.y);
        */

        return std::make_pair(top, bottom);
    }

public:
    static void
    Initialize(Handle<Object> target)
    {
        HandleScope scope;

        Local<FunctionTemplate> t = FunctionTemplate::New(New);
        t->InstanceTemplate()->SetInternalFieldCount(1);
        NODE_SET_PROTOTYPE_METHOD(t, "push", Push);
        NODE_SET_PROTOTYPE_METHOD(t, "encode", PngEncode);
        NODE_SET_PROTOTYPE_METHOD(t, "dimensions", Dimensions);
        target->Set(String::NewSymbol("DynamicPngStack"), t->GetFunction());
    }

    DynamicPngStack(buffer_type bbuf_type) : buf_type(bbuf_type) {}

    ~DynamicPngStack() {
        for (vPngi it = png_stack.begin(); it != png_stack.end(); ++it) {
            delete *it;
        }
    }

    Handle<Value>
    Push(Buffer *buf, int x, int y, int w, int h)
    {
        try {
            Png *png = new Png((unsigned char *)buf->data(), buf->length(), x, y, w, h);
            png_stack.push_back(png);
            return Undefined();
        }
        catch (const char *e) {
            return VException(e);
        }
    }

    Handle<Value>
    PngEncode()
    {
        HandleScope scope;

        std::pair<Point, Point> optimal = OptimalDimension();
        Point top = optimal.first, bot = optimal.second;

        // printf("width, height: %d, %d\n", bot.x - top.x, bot.y - top.y);

        offset = top;
        width = bot.x - top.x;
        height = bot.y - top.y;

        unsigned char *data = (unsigned char*)malloc(sizeof(*data) * width * height * 4);
        if (!data) return VException("malloc failed in DynamicPngStack::PngEncode");
        memset(data, 0xFF, width*height*4);

        if (buf_type == BUF_RGB || buf_type == BUF_BGR) {
            for (vPngi it = png_stack.begin(); it != png_stack.end(); ++it) {
                Png *png = *it;
                int start = (png->y - top.y)*width*4 + (png->x - top.x)*4;
                for (int i = 0; i < png->h; i++) {
                    for (int j = 0, k = 0; k < 3*png->w; j+=4, k+=3) {
                        data[start + i*width*4 + j] = png->data[i*png->w*3 + k];
                        data[start + i*width*4 + j + 1] = png->data[i*png->w*3 + k + 1];
                        data[start + i*width*4 + j + 2] = png->data[i*png->w*3 + k + 2];
                        data[start + i*width*4 + j + 3] = 0x00;
                    }
                }
            }
        }
        else {
            for (vPngi it = png_stack.begin(); it != png_stack.end(); ++it) {
                Png *png = *it;
                int start = (png->y - top.y)*width*4 + (png->x - top.x)*4;
                for (int i = 0; i < png->h; i++) {
                    for (int j = 0; j < 4*png->w; j+=4) {
                        data[start + i*width*4 + j] = png->data[i*png->w*4 + j];
                        data[start + i*width*4 + j + 1] = png->data[i*png->w*4 + j + 1];
                        data[start + i*width*4 + j + 2] = png->data[i*png->w*4 + j + 2];
                        data[start + i*width*4 + j + 3] = png->data[i*png->w*4 + j + 3];
                    }
                }
            }
        }

        buffer_type pbt = BUF_RGBA;
        if (buf_type == BUF_BGR || buf_type == BUF_BGRA)
            pbt = BUF_BGRA;

        PngEncoder p(data, width, height, pbt);
        Handle<Value> ret = p.encode();
        free(data);
        if (!ret->IsUndefined()) return ret;
        return scope.Close(Encode((char *)p.get_png(), p.get_png_len(), BINARY));
    }

    Handle<Value>
    Dimensions()
    {
        HandleScope scope;

        Local<Object> dim = Object::New();
        dim->Set(String::NewSymbol("x"), Integer::New(offset.x));
        dim->Set(String::NewSymbol("y"), Integer::New(offset.y));
        dim->Set(String::NewSymbol("width"), Integer::New(width));
        dim->Set(String::NewSymbol("height"), Integer::New(height));
        return scope.Close(dim);
    }

protected:
    static Handle<Value>
    New(const Arguments &args)
    {
        HandleScope scope;

        buffer_type buf_type = BUF_RGB;
        if (args.Length() == 1) {
            if (!args[0]->IsString())
                return VException("First argument must be 'rgb', 'bgr', 'rgba' or 'bgra'.");

            String::AsciiValue bts(args[0]->ToString());
            if (!(str_eq(*bts, "rgb") || str_eq(*bts, "bgr") ||
                str_eq(*bts, "rgba") || str_eq(*bts, "bgra")))
            {
                return VException("First argument must be 'rgb', 'bgr', 'rgba' or 'bgra'.");
            }
            
            if (str_eq(*bts, "rgb"))
                buf_type = BUF_RGB;
            else if (str_eq(*bts, "bgr"))
                buf_type = BUF_BGR;
            else if (str_eq(*bts, "rgba"))
                buf_type = BUF_RGBA;
            else if (str_eq(*bts, "bgra"))
                buf_type = BUF_BGRA;
            else
                return VException("First argument wasn't 'rgb', 'bgr', 'rgba' or 'bgra'.");
        }

        DynamicPngStack *png_stack = new DynamicPngStack(buf_type);
        png_stack->Wrap(args.This());
        return args.This();
    }

    static Handle<Value>
    Push(const Arguments &args)
    {
        HandleScope scope;

        if (!Buffer::HasInstance(args[0]))
            return VException("First argument must be Buffer.");
        if (!args[1]->IsInt32())
            return VException("Second argument must be integer x.");
        if (!args[2]->IsInt32())
            return VException("Third argument must be integer y.");
        if (!args[3]->IsInt32())
            return VException("Fourth argument must be integer w.");
        if (!args[4]->IsInt32())
            return VException("Fifth argument must be integer h.");

        Buffer *data = ObjectWrap::Unwrap<Buffer>(args[0]->ToObject());
        int x = args[1]->Int32Value();
        int y = args[2]->Int32Value();
        int w = args[3]->Int32Value();
        int h = args[4]->Int32Value();

        if (x < 0)
            return VException("Coordinate x smaller than 0.");
        if (y < 0)
            return VException("Coordinate y smaller than 0.");
        if (w < 0)
            return VException("Width smaller than 0.");
        if (h < 0)
            return VException("Height smaller than 0.");

        DynamicPngStack *png_stack = ObjectWrap::Unwrap<DynamicPngStack>(args.This());
        return scope.Close(png_stack->Push(data, x, y, w, h));
    }

    static Handle<Value>
    Dimensions(const Arguments &args)
    {
        HandleScope scope;

        DynamicPngStack *png_stack = ObjectWrap::Unwrap<DynamicPngStack>(args.This());
        return scope.Close(png_stack->Dimensions());
    }

    static Handle<Value>
    PngEncode(const Arguments &args)
    {
        HandleScope scope;

        DynamicPngStack *png_stack = ObjectWrap::Unwrap<DynamicPngStack>(args.This());
        return scope.Close(png_stack->PngEncode());
    }
};

extern "C" void
init(Handle<Object> target)
{
    HandleScope scope;
    Png::Initialize(target);
    FixedPngStack::Initialize(target);
    DynamicPngStack::Initialize(target);
}

