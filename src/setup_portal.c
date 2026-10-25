#include "setup_portal.h"
#include "wifi_manager.h"
#include "sonos_controller.h"
#include "app_config.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "setup_portal";

static httpd_handle_t s_srv      = NULL;
static TaskHandle_t   s_dns_task = NULL;
static volatile bool  s_done     = false;

// Remembered between a successful /api/connect and the final /api/finish,
// so the wizard's last step doesn't need the password resent (and so we
// never persist unverified credentials to NVS — only a password that
// actually worked gets saved, by /api/finish, not /api/connect itself).
static char s_pending_ssid[33]     = {0};
static char s_pending_password[64] = {0};

// ---- Wizard page --------------------------------------------------------
// Background/ink colors match the device's own vinyl artwork exactly
// (#E4E1D0 / #363531 — same values main.c uses for the spinner/version
// label). No real internet reaches a phone joined to this AP, so Google
// Fonts etc. aren't an option for actual Montserrat — a system sans-serif
// stack with added letter-spacing approximates its spaced-out uppercase
// look instead.
static const char WIZARD_HTML[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Jukebox Jam Setup</title><style>"
":root{--bg:#E4E1D0;--ink:#363531}"
"*{box-sizing:border-box}"
"body{margin:0;background:var(--bg);color:var(--ink);min-height:100vh;"
"font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;"
"letter-spacing:.03em;display:flex;flex-direction:column;align-items:center;"
"padding:28px 20px 60px}"
"h1{font-size:1.15rem;text-transform:uppercase;letter-spacing:.14em;"
"font-weight:600;margin:0 0 2px}"
"p.sub{margin:0 0 26px;opacity:.65;font-size:.8rem;letter-spacing:.1em;"
"text-transform:uppercase}"
"h2{font-size:.85rem;text-transform:uppercase;letter-spacing:.1em;"
"font-weight:600;margin:0 0 14px}"
".card{width:100%;max-width:360px}"
".step{display:none}.step.active{display:block}"
"button{width:100%;padding:14px;border:none;border-radius:10px;"
"background:var(--ink);color:var(--bg);font-size:.95rem;letter-spacing:.08em;"
"text-transform:uppercase;font-weight:600;cursor:pointer;margin-top:12px}"
"button:disabled{opacity:.45}"
"button.secondary{background:transparent;border:1.5px solid var(--ink);color:var(--ink)}"
"input{width:100%;padding:12px;border-radius:8px;border:1.5px solid "
"rgba(54,53,49,.35);background:rgba(255,255,255,.3);color:var(--ink);"
"font-size:1rem;margin-top:4px;letter-spacing:.02em}"
".row{padding:13px 14px;border:1.5px solid rgba(54,53,49,.22);border-radius:8px;"
"margin-top:8px;display:flex;justify-content:space-between;align-items:center;"
"cursor:pointer;font-size:.9rem}"
".row.selected{border-color:var(--ink);background:rgba(54,53,49,.08)}"
".row .meta{opacity:.55;font-size:.75rem;margin-left:10px;white-space:nowrap}"
".msg{margin-top:14px;font-size:.82rem;letter-spacing:.03em;text-align:center}"
".msg.err{color:#8a3b2f}"
".label{font-size:.78rem;opacity:.7;letter-spacing:.06em;text-transform:uppercase;"
"margin-top:16px;display:block}"
"</style></head><body>"
"<h1>Jukebox Jam</h1><p class='sub'>Setup</p>"
"<div class='card'>"

"<div id='step-scan' class='step active'>"
"<h2>Step 1 &middot; WiFi Network</h2>"
"<button id='scan-btn' onclick='doScan()'>Scan For Networks</button>"
"<div id='net-list'></div>"
"</div>"

"<div id='step-pass' class='step'>"
"<h2>Step 2 &middot; Connect</h2>"
"<span class='label' id='ssid-label'></span>"
"<input type='password' id='pass' placeholder='WiFi password'>"
"<button id='connect-btn' onclick='doConnect()'>Connect</button>"
"<button class='secondary' onclick='show(\"step-scan\")'>Back</button>"
"<div id='connect-msg' class='msg'></div>"
"</div>"

"<div id='step-speaker' class='step'>"
"<h2>Step 3 &middot; Choose Speaker</h2>"
"<div id='speaker-list'></div>"
"<button id='finish-btn' onclick='doFinish()' disabled>Finish</button>"
"</div>"

"<div id='step-done' class='step'>"
"<h2>All Set</h2>"
"<div class='msg'>Saved. The device will connect shortly &mdash; you can close this page.</div>"
"</div>"

"</div><script>"
"var chosenSsid='',chosenSpeaker='';"
"function show(id){document.querySelectorAll('.step').forEach(function(s){s.classList.remove('active')});"
"document.getElementById(id).classList.add('active')}"
"function doScan(){"
"var b=document.getElementById('scan-btn');b.textContent='Scanning…';b.disabled=true;"
"fetch('/api/scan').then(function(r){return r.json()}).then(function(nets){"
"var list=document.getElementById('net-list');list.innerHTML='';"
"nets.forEach(function(n){"
"var d=document.createElement('div');d.className='row';"
"d.innerHTML='<span>'+(n.secure?'🔒 ':'')+n.ssid+'</span>"
"<span class=\"meta\">'+n.rssi+'dBm</span>';"
"d.onclick=function(){chosenSsid=n.ssid;"
"document.getElementById('ssid-label').textContent=n.ssid;"
"document.getElementById('pass').value='';"
"document.getElementById('connect-msg').textContent='';"
"show('step-pass')};"
"list.appendChild(d)});"
"b.textContent='Scan Again';b.disabled=false"
"}).catch(function(){b.textContent='Scan Again';b.disabled=false})}"
"function doConnect(){"
"var pass=document.getElementById('pass').value;"
"var b=document.getElementById('connect-btn');b.textContent='Connecting…';b.disabled=true;"
"var m=document.getElementById('connect-msg');m.className='msg';m.textContent='';"
"fetch('/api/connect',{method:'POST',headers:{'Content-Type':'application/json'},"
"body:JSON.stringify({ssid:chosenSsid,password:pass})})"
".then(function(r){return r.json()}).then(function(res){"
"b.textContent='Connect';b.disabled=false;"
"if(res.ok){doSonosScan()}else{m.className='msg err';m.textContent=res.error||'Could not connect'}"
"}).catch(function(){b.textContent='Connect';b.disabled=false;"
"m.className='msg err';m.textContent='Request failed'})}"
"function doSonosScan(){"
"show('step-speaker');"
"var list=document.getElementById('speaker-list');"
"list.innerHTML='<div class=\"msg\">Searching for speakers…</div>';"
"document.getElementById('finish-btn').disabled=true;"
"fetch('/api/sonos/scan').then(function(r){return r.json()}).then(function(res){"
"list.innerHTML='';"
"if(!res.speakers.length){list.innerHTML='<div class=\"msg err\">No speakers found — check the speaker is powered on and try again.</div>';return}"
"res.speakers.forEach(function(s,i){"
"var d=document.createElement('div');"
"d.className='row'+(i===res['default']?' selected':'');"
"d.textContent=s.name;"
"d.onclick=function(){"
"document.querySelectorAll('#speaker-list .row').forEach(function(n){n.classList.remove('selected')});"
"d.classList.add('selected');chosenSpeaker=s.name};"
"list.appendChild(d);"
"if(i===res['default'])chosenSpeaker=s.name"
"});"
"document.getElementById('finish-btn').disabled=false"
"}).catch(function(){list.innerHTML='<div class=\"msg err\">Speaker search failed</div>'})}"
"function doFinish(){"
"var b=document.getElementById('finish-btn');b.textContent='Saving…';b.disabled=true;"
"fetch('/api/finish',{method:'POST',headers:{'Content-Type':'application/json'},"
"body:JSON.stringify({speaker:chosenSpeaker})})"
".then(function(){show('step-done')})"
".catch(function(){b.textContent='Finish';b.disabled=false})}"
"</script></body></html>";

// ---- JSON body helper ---------------------------------------------------

static cJSON *read_json_body(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > 1024) return NULL;
    char *buf = malloc(total + 1);
    if (!buf) return NULL;
    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, buf + received, total - received);
        if (r <= 0) { free(buf); return NULL; }
        received += r;
    }
    buf[total] = '\0';
    cJSON *json = cJSON_Parse(buf);
    free(buf);
    return json;
}

// ---- HTTP handlers --------------------------------------------------------

static esp_err_t h_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, WIZARD_HTML, sizeof(WIZARD_HTML) - 1);
}

// Every other GET/HEAD — the DNS hijack sends a phone here for whatever
// domain its OS connectivity-check probed (generate_204, hotspot-detect,
// connecttest.txt, ...); redirecting all of them to "/" is what makes the
// portal auto-pop instead of the phone reporting "no internet" and
// giving up. HEAD matters too — Windows' NCSI probe and some browsers'
// pre-flight checks use it, and httpd matches (uri, method) pairs, so a
// bare 404 on HEAD reads as "no captive portal here" to some OSes.
static esp_err_t h_redirect(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" WIFI_AP_IP "/");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t h_scan(httpd_req_t *req)
{
    wifi_scan_result_t nets[24];
    int n = wifi_manager_scan(nets, 24);

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", nets[i].ssid);
        cJSON_AddNumberToObject(o, "rssi", nets[i].rssi);
        cJSON_AddBoolToObject(o, "secure", nets[i].secure);
        cJSON_AddItemToArray(arr, o);
    }
    char *text = cJSON_PrintUnformatted(arr);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, text);
    free(text);
    cJSON_Delete(arr);
    return ESP_OK;
}

static esp_err_t h_connect(httpd_req_t *req)
{
    cJSON *body = read_json_body(req);
    cJSON *jssid = body ? cJSON_GetObjectItem(body, "ssid") : NULL;
    cJSON *jpass = body ? cJSON_GetObjectItem(body, "password") : NULL;
    const char *ssid = cJSON_IsString(jssid) ? jssid->valuestring : NULL;
    const char *pass = cJSON_IsString(jpass) ? jpass->valuestring : "";

    cJSON *resp = cJSON_CreateObject();
    char ip[24] = {0};
    if (!ssid || !ssid[0]) {
        cJSON_AddBoolToObject(resp, "ok", false);
        cJSON_AddStringToObject(resp, "error", "No network selected");
    } else if (wifi_manager_test_connect(ssid, pass, 15000, ip, sizeof(ip))) {
        strncpy(s_pending_ssid, ssid, sizeof(s_pending_ssid) - 1);
        strncpy(s_pending_password, pass, sizeof(s_pending_password) - 1);
        cJSON_AddBoolToObject(resp, "ok", true);
        cJSON_AddStringToObject(resp, "ip", ip);
    } else {
        cJSON_AddBoolToObject(resp, "ok", false);
        cJSON_AddStringToObject(resp, "error", "Could not connect — check the password");
    }
    if (body) cJSON_Delete(body);

    char *text = cJSON_PrintUnformatted(resp);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, text);
    free(text);
    cJSON_Delete(resp);
    return ESP_OK;
}

static esp_err_t h_sonos_scan(httpd_req_t *req)
{
    sonos_controller_discover(SONOS_DISCOVERY_TIMEOUT_MS);

    sonos_speaker_info_t speakers[SONOS_MAX_SPEAKERS];
    int default_idx = 0;
    int n = sonos_controller_list_speakers(speakers, SONOS_MAX_SPEAKERS, &default_idx);

    cJSON *resp = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", speakers[i].name);
        cJSON_AddStringToObject(o, "ip", speakers[i].ip);
        cJSON_AddItemToArray(arr, o);
    }
    cJSON_AddItemToObject(resp, "speakers", arr);
    cJSON_AddNumberToObject(resp, "default", n > 0 ? default_idx : 0);

    char *text = cJSON_PrintUnformatted(resp);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, text);
    free(text);
    cJSON_Delete(resp);
    return ESP_OK;
}

static void restart_task(void *arg)
{
    // Long enough for the HTTP response below to actually flush over the
    // (soon to be torn-down) AP connection, and for the wizard's JS to
    // show the "All Set" step, before the reboot cuts everything off.
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

static esp_err_t h_finish(httpd_req_t *req)
{
    cJSON *body = read_json_body(req);
    cJSON *jspeaker = body ? cJSON_GetObjectItem(body, "speaker") : NULL;
    const char *speaker = cJSON_IsString(jspeaker) ? jspeaker->valuestring : NULL;

    if (speaker && speaker[0]) sonos_controller_select_speaker(speaker);
    // Only ever persists a password that a real test_connect already
    // verified (see h_connect) — never an unverified one.
    if (s_pending_ssid[0]) wifi_manager_save_credentials(s_pending_ssid, s_pending_password);

    if (body) cJSON_Delete(body);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");

    // A clean reboot, rather than trying to gracefully drop setup-AP mode
    // and reconnect within the same running WiFi driver instance —
    // confirmed on real hardware that the STA stayed silently associated
    // from this wizard's own test-connect (h_connect) the whole time
    // since nothing ever explicitly disconnected it, so a later
    // esp_wifi_connect() with the same config was a no-op: no new
    // disconnect or GOT_IP event ever fired, and the normal reconnect
    // path just sat there until timeout, stuck on the Boot screen.
    // Deferred to its own task for the same reason as the flag below —
    // esp_restart() itself doesn't deadlock, but giving the HTTP response
    // a moment to actually leave first (see restart_task) does need to
    // happen off this handler's own return path.
    s_done = true;
    xTaskCreate(restart_task, "setup_restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

// ---- DNS hijack -----------------------------------------------------------
// Responds to every DNS A query with the AP's own IP, so any domain a
// phone's OS probes (or the user types) resolves here and the HTTP
// catch-all redirect takes it to the wizard. Ported from the sibling
// Grill & Groove project's wifi_manager.c, confirmed on real hardware.

static void dns_server_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { vTaskDelete(NULL); return; }

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    uint8_t buf[512];
    struct sockaddr_in client;
    socklen_t client_len = sizeof(client);

    for (;;) {
        int len = recvfrom(sock, buf, sizeof(buf) - 16, 0, (struct sockaddr *)&client, &client_len);
        if (len < 12) continue;

        uint8_t resp[512];
        if (len + 16 > (int)sizeof(resp)) continue;
        memcpy(resp, buf, len);

        resp[2] = 0x81;   // QR=1, Opcode=0, AA=1
        resp[3] = 0x80;   // RA=1, no error
        resp[6] = 0x00; resp[7] = 0x01;   // 1 answer RR
        resp[8] = 0x00; resp[9] = 0x00;   // 0 authority RRs
        resp[10] = 0x00; resp[11] = 0x00; // 0 additional RRs

        int pos = len;
        resp[pos++] = 0xC0; resp[pos++] = 0x0C;   // name -> offset 12 (question name)
        resp[pos++] = 0x00; resp[pos++] = 0x01;   // TYPE A
        resp[pos++] = 0x00; resp[pos++] = 0x01;   // CLASS IN
        resp[pos++] = 0x00; resp[pos++] = 0x00;
        resp[pos++] = 0x00; resp[pos++] = 0x3C;   // TTL 60s
        resp[pos++] = 0x00; resp[pos++] = 0x04;   // RDLENGTH 4
        resp[pos++] = 192; resp[pos++] = 168;
        resp[pos++] = 4;   resp[pos++] = 1;

        sendto(sock, resp, pos, 0, (struct sockaddr *)&client, client_len);
    }
}

// ---- Lifecycle --------------------------------------------------------

void setup_portal_start(void)
{
    if (s_srv) return;
    s_done = false;
    s_pending_ssid[0] = s_pending_password[0] = '\0';

    wifi_manager_start_setup_ap();

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn     = httpd_uri_match_wildcard;
    cfg.max_uri_handlers  = 8;
    // h_sonos_scan() calls straight into sonos_controller_discover(),
    // which alone uses 2KB+ of its own stack buffers (the same reason
    // boot_task needed an 8192-byte stack, in main.c) — well beyond
    // httpd's small default worker stack.
    cfg.stack_size = 8192;
    // The DNS hijack sends every domain here, so a phone's parallel
    // connectivity-check probes can pile up fast — lru_purge_enable makes
    // a full server evict its oldest connection instead of permanently
    // failing accept() once the socket pool fills (seen on the sibling
    // project as endless "httpd_accept_conn: error in accept" that never
    // recovered without this).
    cfg.max_open_sockets = 9;
    cfg.lru_purge_enable = true;
    if (httpd_start(&s_srv, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return;
    }

    // Specific routes first — the wildcard catch-all must be registered
    // last, and needs both GET and HEAD (see h_redirect's comment).
    static const httpd_uri_t routes[] = {
        { .uri = "/",              .method = HTTP_GET,  .handler = h_index      },
        { .uri = "/api/scan",      .method = HTTP_GET,  .handler = h_scan       },
        { .uri = "/api/connect",   .method = HTTP_POST, .handler = h_connect    },
        { .uri = "/api/sonos/scan",.method = HTTP_GET,  .handler = h_sonos_scan },
        { .uri = "/api/finish",    .method = HTTP_POST, .handler = h_finish     },
        { .uri = "/*",             .method = HTTP_GET,  .handler = h_redirect   },
        { .uri = "/*",             .method = HTTP_HEAD, .handler = h_redirect   },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(s_srv, &routes[i]);
    }

    xTaskCreate(dns_server_task, "setup_dns", 3072, NULL, 5, &s_dns_task);
    ESP_LOGI(TAG, "portal up at http://" WIFI_AP_IP "/");
}

void setup_portal_stop(void)
{
    if (s_dns_task) { vTaskDelete(s_dns_task); s_dns_task = NULL; }
    if (s_srv) { httpd_stop(s_srv); s_srv = NULL; }
    wifi_manager_stop_setup_ap();
}

bool setup_portal_is_done(void)
{
    return s_done;
}

const char *setup_portal_qr_data(void)
{
    // WIFI_AP_SSID is a fixed literal with no characters needing escaping
    // in the WIFI: QR scheme (no ';', ',', ':', '\', '"') — if it's ever
    // changed to something that does, this needs the standard backslash
    // escaping for those characters.
    return "WIFI:T:nopass;S:" WIFI_AP_SSID ";;";
}
