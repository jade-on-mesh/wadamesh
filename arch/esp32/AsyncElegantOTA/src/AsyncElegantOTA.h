#ifndef AsyncElegantOTA_h
#define AsyncElegantOTA_h

#warning AsyncElegantOTA library is deprecated, Please consider moving to newer ElegantOTA library which now comes with an Async Mode. Learn More: https://docs.elegantota.pro/async-mode/ 

#include "Arduino.h"
#include "stdlib_noniso.h"

#if defined(ESP8266)
    #include "ESP8266WiFi.h"
    #include "ESPAsyncTCP.h"
    #include "flash_hal.h"
    #include "FS.h"
#elif defined(ESP32)
    #include "WiFi.h"
    #include "AsyncTCP.h"
    #include "Update.h"
#endif

#include "Hash.h"
#include "ESPAsyncWebServer.h"
#include "FS.h"

#if defined(ELEGANT_OTA_LITE)
// A plain upload page in place of the 53 KB gzipped ElegantOTA app, for boards
// whose app partition is full (the T-Deck). Same /update endpoint and fields.
static const char ELEGANT_LITE_HTML[] PROGMEM = R"HTML(<!doctype html>
<meta name=viewport content="width=device-width,initial-scale=1"><title>Firmware update</title>
<style>body{font:16px system-ui,sans-serif;max-width:28em;margin:2em auto;padding:0 1em}progress{width:100%}</style>
<h2>Firmware update</h2>
<p>Choose a firmware .bin for this device.</p>
<input type=file id=f accept=".bin"> <button id=b>Upload</button>
<p><progress id=p max=100 value=0></progress></p><p id=s></p>
<script>
b.onclick=function(){var x=f.files[0];if(!x)return;var d=new FormData();d.append('firmware',x,x.name);
var r=new XMLHttpRequest();r.open('POST','/update');b.disabled=1;s.textContent='Uploading...';
r.upload.onprogress=function(e){if(e.lengthComputable)p.value=e.loaded*100/e.total};
r.onload=function(){s.textContent=r.status==200?'Done. The device restarts.':'Failed: '+r.responseText;b.disabled=0};
r.onerror=function(){s.textContent='Connection lost (the device may be restarting).'};r.send(d)}
</script>)HTML";
#else
#include "elegantWebpage.h"
#endif


class AsyncElegantOtaClass{

    public:
        void
            setID(const char* id),
            begin(AsyncWebServer *server, const char* username = "", const char* password = ""),
            loop(),
            restart();

    private:
        AsyncWebServer *_server;

        String getID();

        String _id = getID();
        String _username = "";
        String _password = "";
        bool _authRequired = false;

};

extern AsyncElegantOtaClass AsyncElegantOTA;

#endif
