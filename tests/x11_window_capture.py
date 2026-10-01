"""Capture an owned, composited X11 window without reading covering windows."""
import ctypes as C
import ctypes.util
import time
from PIL import Image

class XImage(C.Structure):
    _fields_=[('width',C.c_int),('height',C.c_int),('xoffset',C.c_int),('format',C.c_int),
              ('data',C.c_void_p),('byte_order',C.c_int),('bitmap_unit',C.c_int),
              ('bitmap_bit_order',C.c_int),('bitmap_pad',C.c_int),('depth',C.c_int),
              ('bytes_per_line',C.c_int),('bits_per_pixel',C.c_int),
              ('red_mask',C.c_ulong),('green_mask',C.c_ulong),('blue_mask',C.c_ulong)]

class Visual(C.Structure):
    _fields_=[('ext_data',C.c_void_p),('visualid',C.c_ulong),('visual_class',C.c_int),
              ('red_mask',C.c_ulong),('green_mask',C.c_ulong),('blue_mask',C.c_ulong),
              ('bits_per_rgb',C.c_int),('map_entries',C.c_int)]

class XWindowAttributes(C.Structure):
    _fields_=[(n,C.c_int) for n in ('x','y','width','height','border_width','depth')]+[
        ('visual',C.POINTER(Visual)),('root',C.c_ulong),('window_class',C.c_int),
        ('bit_gravity',C.c_int),('win_gravity',C.c_int),('backing_store',C.c_int),
        ('backing_planes',C.c_ulong),('backing_pixel',C.c_ulong),('save_under',C.c_int),
        ('colormap',C.c_ulong),('map_installed',C.c_int),('map_state',C.c_int),
        ('all_event_masks',C.c_long),('your_event_mask',C.c_long),('do_not_propagate_mask',C.c_long),
        ('override_redirect',C.c_int),('screen',C.c_void_p)]

def capture(window,width,height,display=':0'):
    x=C.CDLL(ctypes.util.find_library('X11'));composite=C.CDLL(ctypes.util.find_library('Xcomposite'))
    x.XOpenDisplay.argtypes=[C.c_char_p];x.XOpenDisplay.restype=C.c_void_p
    x.XCloseDisplay.argtypes=[C.c_void_p];x.XSync.argtypes=[C.c_void_p,C.c_int]
    x.XFreePixmap.argtypes=[C.c_void_p,C.c_ulong]
    x.XGetImage.argtypes=[C.c_void_p,C.c_ulong,C.c_int,C.c_int,C.c_uint,C.c_uint,C.c_ulong,C.c_int]
    x.XGetImage.restype=C.POINTER(XImage);x.XDestroyImage.argtypes=[C.POINTER(XImage)]
    x.XGetWindowAttributes.argtypes=[C.c_void_p,C.c_ulong,C.POINTER(XWindowAttributes)]
    composite.XCompositeNameWindowPixmap.argtypes=[C.c_void_p,C.c_ulong]
    composite.XCompositeNameWindowPixmap.restype=C.c_ulong
    composite.XCompositeRedirectWindow.argtypes=[C.c_void_p,C.c_ulong,C.c_int]
    composite.XCompositeUnredirectWindow.argtypes=[C.c_void_p,C.c_ulong,C.c_int]
    handler_type=C.CFUNCTYPE(C.c_int,C.c_void_p,C.c_void_p);errors=[]
    handler=handler_type(lambda _display,_event:errors.append(True) or 0)
    x.XSetErrorHandler.argtypes=[C.c_void_p];x.XSetErrorHandler.restype=C.c_void_p
    previous=x.XSetErrorHandler(C.cast(handler,C.c_void_p));d=x.XOpenDisplay(display.encode())
    pixmap=0;image=None;redirected=False
    try:
        if not d:raise RuntimeError('cannot open X11 display')
        pixmap=composite.XCompositeNameWindowPixmap(d,int(window));x.XSync(d,0)
        if errors:
            pixmap=0;errors.clear()
            # A non-compositing WM has no backing pixmap yet. Redirect only
            # this caller-owned test window, keeping automatic screen updates.
            composite.XCompositeRedirectWindow(d,int(window),0);x.XSync(d,0)
            if errors:raise RuntimeError('cannot redirect owned window for capture')
            redirected=True;time.sleep(.15)
            pixmap=composite.XCompositeNameWindowPixmap(d,int(window));x.XSync(d,0)
            if errors:pixmap=0;raise RuntimeError('owned window has no composite backing pixmap')
        image=x.XGetImage(d,pixmap,0,0,width,height,C.c_ulong(-1).value,2);x.XSync(d,0)
        if errors or not image:raise RuntimeError('cannot read owned window backing pixmap')
        info=image.contents
        attributes=XWindowAttributes()
        if not x.XGetWindowAttributes(d,int(window),C.byref(attributes)) or not attributes.visual:
            raise RuntimeError('cannot read owned window visual')
        visual=attributes.visual.contents
        if (info.byte_order,info.bits_per_pixel,visual.red_mask,visual.green_mask,visual.blue_mask)!=(0,32,0xff0000,0xff00,0xff):
            raise RuntimeError(f'unsupported X11 pixel layout: order={info.byte_order} bpp={info.bits_per_pixel} depth={info.depth} masks={info.red_mask:x}/{info.green_mask:x}/{info.blue_mask:x}')
        data=C.string_at(info.data,info.bytes_per_line*height)
        return Image.frombytes('RGB',(width,height),data,'raw','BGRX',info.bytes_per_line,1)
    finally:
        if image:x.XDestroyImage(image)
        if pixmap:x.XFreePixmap(d,pixmap)
        if redirected:composite.XCompositeUnredirectWindow(d,int(window),0)
        if d:x.XCloseDisplay(d)
        x.XSetErrorHandler(previous)
