////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2016 ArangoDB GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Andreas Streichardt
/// @author Frank Celler
/// @author Jan Christoph Uhde
////////////////////////////////////////////////////////////////////////////////

#include "HttpCommunicator.h"
#include <fcntl.h>
#include <velocypack/Parser.h>
#include <cassert>
#include <sstream>
#include <atomic>
#include <cassert>

#include <fuerte/helper.h>

namespace arangodb {
namespace fuerte {
inline namespace v1 {
namespace http {
// -----------------------------------------------------------------------------
// --SECTION--                                      constructors and destructors
// -----------------------------------------------------------------------------

std::mutex HttpCommunicator::_curlLock;

HttpCommunicator::HttpCommunicator() : _curl(nullptr), _useCount(0) {
  curl_global_init(CURL_GLOBAL_ALL);
  _curl = curl_multi_init();

#ifdef _WIN32
  int err = dumb_socketpair(_socks, 0);
  if (err != 0) {
    throw std::runtime_error("Couldn't setup sockets. Error was: " +
                             std::to_string(err));
  }
  _wakeup.fd = _socks[0];
#else
  int result = pipe(_fds);
  if (result != 0) {
    throw std::runtime_error("Couldn't setup pipe. Return code was: " +
                             std::to_string(result));
  }

  long flags = fcntl(_fds[0], F_GETFL, 0);

  if (flags < 0) {
    throw std::runtime_error(
        "Couldn't set pipe to non-blocking. Return code was: " +
        std::to_string(flags));
  }

  flags = fcntl(_fds[0], F_SETFL, flags | O_NONBLOCK);

  if (flags < 0) {
    throw std::runtime_error(
        "Couldn't set pipe to non-blocking. Return code was: " +
        std::to_string(flags));
  }

  _wakeup.fd = _fds[0];
#endif

  _wakeup.events = CURL_WAIT_POLLIN | CURL_WAIT_POLLPRI;
}

HttpCommunicator::~HttpCommunicator() {
  FUERTE_LOG_HTTPTRACE << "DESTROYING COMMUNICATOR" << std::endl;
  if(! _handlesInProgress.empty()){
    FUERTE_LOG_HTTPTRACE << "DESTROYING CONNECTION WITH: "
                         << _handlesInProgress.size()
                         << " outstanding requests!"
                         << std::endl;
  }
  ::curl_multi_cleanup(_curl);
  ::curl_global_cleanup();
}

// -----------------------------------------------------------------------------
// --SECTION--                                                    public methods
// -----------------------------------------------------------------------------

uint64_t HttpCommunicator::queueRequest(Destination destination,
                                    std::unique_ptr<Request> request,
                                    Callbacks callbacks) {
  FUERTE_LOG_HTTPTRACE << "queueRequest - start - at address: " << request.get() << std::endl;
  static std::atomic<uint64_t> ticketId(0);
      //std::chrono::duration_cast<std::chrono::microseconds>(
      //    std::chrono::steady_clock::now().time_since_epoch())
      //    .count();
  NewRequest newRequest;
  newRequest._destination = destination;
  newRequest._fuRequest = std::move(request);
  newRequest._callbacks = callbacks;

  {
    std::lock_guard<std::mutex> guard(_newRequestsLock);
    uint64_t thisId = ++ticketId;
    newRequest._fuRequest->messageid = thisId;
    _newRequests.emplace_back(std::move(newRequest));
    FUERTE_LOG_HTTPTRACE << "queueRequest - end" << std::endl;
    return thisId;
  }
}

int HttpCommunicator::workOnce() {
  std::lock_guard<std::mutex> guard(_curlLock);

  FUERTE_LOG_DEBUG << "fuerte - HttpCommunicator: work once" << std::endl;
  std::vector<NewRequest> newRequests;

  {
    std::lock_guard<std::mutex> guard(_newRequestsLock);
    newRequests.swap(_newRequests);
  }

  for (auto&& newRequest : newRequests) {
    createRequestInProgress(std::move(newRequest));

    FUERTE_LOG_HTTPTRACE << "CREATE REQUEST\n";
  }

  _mc = curl_multi_perform(_curl, &_stillRunning);

  if (_mc != CURLM_OK) {
    throw std::runtime_error(
        "Invalid curl multi result while performing! Result was " +
        std::to_string(_mc));
  }

  // handle all messages received
  CURLMsg* msg = nullptr;
  int msgsLeft = 0;

  while ((msg = curl_multi_info_read(_curl, &msgsLeft))) {
    if (msg->msg == CURLMSG_DONE) {
      CURL* handle = msg->easy_handle;

      handleResult(handle, msg->data.result);
    }
  }

  return _stillRunning;
}

void HttpCommunicator::wait() {
  static int const MAX_WAIT_MSECS = 1000;  // wait max. 1 seconds

  int numFds;  // not used here
  int res = curl_multi_wait(_curl, &_wakeup, 1, MAX_WAIT_MSECS, &numFds);

  if (res != CURLM_OK) {
    throw std::runtime_error(
        "Invalid curl multi result while waiting! Result was " +
        std::to_string(res));
  }
}

// -----------------------------------------------------------------------------
// --SECTION--                                                   private methods
// -----------------------------------------------------------------------------

int HttpCommunicator::curlDebug(CURL* handle, curl_infotype type, char* data,
                                size_t size, void* userptr) {
  RequestInProgress* inprogress = nullptr;
  curl_easy_getinfo(handle, CURLINFO_PRIVATE, &inprogress);

  std::string dataStr(data, size);
  std::string prefix("Communicator(" +
                     std::to_string(inprogress->_request._fuRequest->messageid) + "): ");

  switch (type) {
    case CURLINFO_TEXT:
      FUERTE_LOG_HTTPTRACE << prefix << "Text: " << dataStr;
      break;

    case CURLINFO_HEADER_OUT:
      logHttpHeaders(prefix + "Header >>", dataStr);
      break;

    case CURLINFO_HEADER_IN:
      logHttpHeaders(prefix + "Header <<", dataStr);
      break;

    case CURLINFO_DATA_OUT:
    case CURLINFO_SSL_DATA_OUT:
      logHttpBody(prefix + "Body >>", dataStr);
      break;

    case CURLINFO_DATA_IN:
    case CURLINFO_SSL_DATA_IN:
      logHttpBody(prefix + "Body <<", dataStr);
      break;

    case CURLINFO_END:
      break;
  }

  return 0;
}

void HttpCommunicator::logHttpBody(std::string const& prefix,
                                   std::string const& data) {
  std::string::size_type n = 0;

  while (n < data.length()) {
    FUERTE_LOG_HTTPTRACE << prefix << " " << data.substr(n, 80) << "\n";
    n += 80;
  }
}

void HttpCommunicator::logHttpHeaders(std::string const& prefix,
                                      std::string const& headerData) {
  std::string::size_type last = 0;
  std::string::size_type n;

  while (true) {
    n = headerData.find("\r\n", last);

    if (n == std::string::npos) {
      break;
    }

    FUERTE_LOG_HTTPTRACE << prefix << " "
                         << headerData.substr(last, n - last)
                         << "\n";
    last = n + 2;
  }
}

// stores headers in _responseHeaders (lowercase)
size_t HttpCommunicator::readHeaders(char* buffer, size_t size, size_t nitems,
                                     void* userptr) {
  size_t realsize = size * nitems;
  RequestInProgress* rip = (struct RequestInProgress*)userptr;

  std::string const header(buffer, realsize);
  size_t pivot = header.find_first_of(':');

  if (pivot != std::string::npos) {
    std::string key(header.c_str(), pivot);
    std::transform(key.begin(), key.end(), key.begin(), ::tolower);

    rip->_responseHeaders.emplace(
        key, header.substr(pivot + 2, header.length() - pivot - 4));
  }
  return realsize;
}

size_t HttpCommunicator::readBody(void* data, size_t size, size_t nitems,
                                  void* userp) {
  size_t realsize = size * nitems;

  RequestInProgress* rip = (struct RequestInProgress*)userp;

  try {
    rip->_responseBody.append((char*)data, realsize);
    return realsize;
  } catch (std::bad_alloc&) {
    return 0;
  }
}

void HttpCommunicator::transformResult(CURL* handle, mapss&& responseHeaders,
                                       std::string const& responseBody,
                                       Response* response) {
#if  ENABLE_FUERTE_LOG_HTTPTRACE > 0
  std::cout << "header START" << std::endl;
  for(auto& p : responseHeaders){
    std::cout << p.first << "  " <<p.second << std::endl;
  }
  std::cout << "header END" << std::endl;
#endif

  // no available - response->header.requestType
  auto const& ctype = responseHeaders[fu_content_type_key];
  response->header.contentType(ctype);

  if(responseBody.length()){
    switch (response->contentType()){

      // At the moment we can hadle only one slice
      // in the response
      case ContentType::VPack: {
        auto slice = VSlice(responseBody.c_str());
        response->addVPack(slice);
        break;
      }

      default: {
        VBuffer buffer;
        buffer.append(responseBody);
        response->addBinarySingle(std::move(buffer));
        break;
      }

    }
  }
  response->header.meta = std::move(responseHeaders);

}

std::string HttpCommunicator::createSafeDottedCurlUrl(
    std::string const& originalUrl) {
  std::string url;
  url.reserve(originalUrl.length());

  size_t length = originalUrl.length();
  size_t currentFind = 0;
  std::size_t found;
  std::vector<char> urlDotSeparators{'/', '#', '?'};

  while ((found = originalUrl.find("/.", currentFind)) != std::string::npos) {
    if (found + 2 == length) {
      url += originalUrl.substr(currentFind, found - currentFind) + "/%2E";
    } else if (std::find(urlDotSeparators.begin(), urlDotSeparators.end(),
                         originalUrl.at(found + 2)) != urlDotSeparators.end()) {
      url += originalUrl.substr(currentFind, found - currentFind) + "/%2E";
    } else {
      url += originalUrl.substr(currentFind, found - currentFind) + "/.";
    }
    currentFind = found + 2;
  }
  url += originalUrl.substr(currentFind);
  return url;
}

void HttpCommunicator::createRequestInProgress(NewRequest newRequest) {
  // mop: the curl handle will be managed safely via unique_ptr and hold
  // ownership for rip
  auto rip = new RequestInProgress(std::move(newRequest));
  std::unique_ptr<CurlHandle> handleInProgress(new CurlHandle(rip));
  fuerte::Request* fuRequest = rip->_request._fuRequest.get();

  CURL* handle = handleInProgress->_handle;
  struct curl_slist* requestHeaders = nullptr;

  if(fuRequest->header.meta){
    for (auto const& header : fuRequest->header.meta.get()) {
      std::string thisHeader(header.first + ": " + header.second);
      requestHeaders = curl_slist_append(requestHeaders, thisHeader.c_str());
    }
  }

  std::string url = createSafeDottedCurlUrl(rip->_request._destination);
  handleInProgress->_rip->_requestHeaders = requestHeaders;
  curl_easy_setopt(handle, CURLOPT_HTTPHEADER, requestHeaders);
  curl_easy_setopt(handle, CURLOPT_HEADER, 0L);
  curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
  curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, HttpCommunicator::readBody);
  curl_easy_setopt(handle, CURLOPT_WRITEDATA, handleInProgress->_rip.get());
  curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION,
                   HttpCommunicator::readHeaders);
  curl_easy_setopt(handle, CURLOPT_HEADERDATA, handleInProgress->_rip.get());
#if ENABLE_FUERTE_LOG_HTTPRACE > 0
  curl_easy_setopt(handle, CURLOPT_DEBUGFUNCTION, HttpCommunicator::curlDebug);
  curl_easy_setopt(handle, CURLOPT_DEBUGDATA, handleInProgress->_rip.get());
  curl_easy_setopt(handle, CURLOPT_VERBOSE, 1L);
#endif
  curl_easy_setopt(handle, CURLOPT_ERRORBUFFER,
                   handleInProgress->_rip.get()->_errorBuffer);

  // mop: XXX :S CURLE 51 and 60...
  curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 0L);
  curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, 0L);

  long connectTimeout =
      static_cast<long>(rip->_request._options.connectionTimeout);

  // mop: although curl is offering a MS scale connecttimeout this gets ignored
  // in at least 7.50.3; in doubt change the timeout to _MS below and hardcode
  // it to 999 and see if the requests immediately fail if not this hack can go
  // away

  if (connectTimeout <= 0) {
    connectTimeout = 1;
  }

  curl_easy_setopt(
      handle, CURLOPT_TIMEOUT_MS,
      static_cast<long>(rip->_request._options.requestTimeout * 1000));
  curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, connectTimeout);

  auto verb = fuRequest->header.restVerb.get();

  switch (verb) {
    case RestVerb::Post:
      curl_easy_setopt(handle, CURLOPT_POST, 1);
      break;

    case RestVerb::Put:
      // mop: apparently CURLOPT_PUT implies more stuff in curl
      // (for example it adds an expect 100 header)
      // this is not what we want so we make it a custom request
      curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, "PUT");
      break;

    case RestVerb::Delete:
      curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, "DELETE");
      break;

    case RestVerb::Head:
      curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, "HEAD");
      break;

    case RestVerb::Patch:
      curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, "PATCH");
      break;

    case RestVerb::Options:
      curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, "OPTIONS");
      break;

    case RestVerb::Get:
      break;

    case RestVerb::Illegal:
      throw std::runtime_error("Invalid request type " + to_string(verb));
      break;
  }

  std::string empty("");
  std::string& body = empty;

  auto pay = fuRequest->payload();

  if (pay.second > 0) {
    // https://curl.haxx.se/libcurl/c/CURLOPT_COPYPOSTFIELDS.html
    // DO NOT CHANGE BODY SIZE LATER!!
    curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE, pay.second);
    curl_easy_setopt(handle, CURLOPT_COPYPOSTFIELDS, pay.first);
  }

  handleInProgress->_rip->_startTime = std::chrono::steady_clock::now();
  _handlesInProgress.emplace(rip->_request._fuRequest->messageid,
                             std::move(handleInProgress));
  curl_multi_add_handle(_curl, handle);
}

void HttpCommunicator::handleResult(CURL* handle, CURLcode rc) {
  // remove request in progress
  curl_multi_remove_handle(_curl, handle);

  RequestInProgress* rip = nullptr;
  curl_easy_getinfo(handle, CURLINFO_PRIVATE, &rip);

  if (rip == nullptr) {
    return;
  }

  std::string prefix("Communicator(" + std::to_string(rip->_request._fuRequest->messageid) +
                     "): ");

  FUERTE_LOG_DEBUG << prefix << "curl rc is : " << rc << " after "
                   << std::chrono::duration_cast<std::chrono::duration<double>>(
                          std::chrono::steady_clock::now() - rip->_startTime)
                          .count()
                   << " s\n";

  if (strlen(rip->_errorBuffer) != 0) {
    FUERTE_LOG_HTTPTRACE << prefix << "curl error details: " << rip->_errorBuffer
                     << "\n";
  }

  auto request_id = rip->_request._fuRequest->messageid;
  try {
    switch (rc) {
      case CURLE_OK: {
        long httpStatusCode = 200;
        curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &httpStatusCode);

        std::unique_ptr<Response> fuResponse(new Response());
        fuResponse->header.responseCode = static_cast<unsigned>(httpStatusCode);
        fuResponse->messageid = rip->_request._fuRequest->messageid;
        transformResult(handle, std::move(rip->_responseHeaders),
                        std::move(rip->_responseBody),
                        dynamic_cast<Response*>(fuResponse.get()));


        auto response_id = fuResponse->messageid;
#if ENABLE_FUERTE_LOG_HTTPTRACE > 0
        std::cout << "CALLING ON SUCESS CALLBACK IN HTTP COMMUNICATOR" << std::endl;
        std::cout << "request id: " << request_id << std::endl;
        std::cout << "response id: " << response_id << std::endl;
        std::cout << "in progress: ";
        for(auto const& item : _handlesInProgress){
          std::cout << item.first << " ";
        }
        std::cout << std::endl << to_string(*rip->_request._fuRequest);
        std::cout << to_string(*fuResponse);
#endif
        rip->_request._callbacks._onSuccess(std::move(rip->_request._fuRequest),
                                            std::move(fuResponse));
        rip->_request._fuRequest = nullptr;
        break;
      }

      case CURLE_COULDNT_CONNECT:
      case CURLE_SSL_CONNECT_ERROR:
      case CURLE_COULDNT_RESOLVE_HOST:
      case CURLE_URL_MALFORMAT:
      case CURLE_SEND_ERROR:
        rip->_request._callbacks._onError(
            static_cast<Error>(ErrorCondition::CouldNotConnect),
            std::move(rip->_request._fuRequest), {nullptr});
        break;

      case CURLE_OPERATION_TIMEDOUT:
      case CURLE_RECV_ERROR:
      case CURLE_GOT_NOTHING:
        rip->_request._callbacks._onError(
            static_cast<Error>(ErrorCondition::Timeout),
            std::move(rip->_request._fuRequest), {nullptr});
        break;

      default:
        FUERTE_LOG_ERROR << "Curl return " << rc << "\n";
        rip->_request._callbacks._onError(
            static_cast<Error>(ErrorCondition::CurlError),
            std::move(rip->_request._fuRequest), {nullptr});
        break;
    }
  } catch (std::exception const& e) {
    _handlesInProgress.erase(request_id);
    throw e;
  }  catch (...) {
    _handlesInProgress.erase(request_id);
    throw;
  }

  _handlesInProgress.erase(request_id);
}
}
}
}
}
