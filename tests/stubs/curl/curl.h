#pragma once
#include <cstddef>
typedef void CURL;
typedef long long curl_off_t;
enum CURLcode { CURLE_OK = 0 };
enum CURLoption { CURLOPT_URL, CURLOPT_USERAGENT, CURLOPT_FOLLOWLOCATION, CURLOPT_MAXREDIRS, CURLOPT_CONNECTTIMEOUT, CURLOPT_NOSIGNAL, CURLOPT_ACCEPT_ENCODING, CURLOPT_TIMEOUT, CURLOPT_LOW_SPEED_LIMIT, CURLOPT_LOW_SPEED_TIME, CURLOPT_CAINFO, CURLOPT_SSL_VERIFYPEER, CURLOPT_SSL_VERIFYHOST, CURLOPT_WRITEFUNCTION, CURLOPT_WRITEDATA, CURLOPT_NOPROGRESS, CURLOPT_XFERINFOFUNCTION, CURLOPT_XFERINFODATA };
enum CURLINFO { CURLINFO_RESPONSE_CODE };
#define CURL_GLOBAL_DEFAULT 0L
CURLcode curl_global_init(long);
void curl_global_cleanup();
CURL* curl_easy_init();
CURLcode curl_easy_setopt(CURL*, CURLoption, ...);
CURLcode curl_easy_perform(CURL*);
CURLcode curl_easy_getinfo(CURL*, CURLINFO, ...);
void curl_easy_cleanup(CURL*);
const char* curl_easy_strerror(CURLcode);
