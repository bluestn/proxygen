/*
 *  Copyright (c) 2018-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include <proxygen/lib/http/codec/HeaderDecodeInfo.h>

using std::string;

namespace proxygen {

bool HeaderDecodeInfo::onHeader(const folly::fbstring& name,
                                const folly::fbstring& value) {
  // Refuse decoding other headers if an error is already found
  if (decodeError != HPACK::DecodeError::NONE
      || parsingError != "") {
    VLOG(4) << "Ignoring header=" << name << " value=" << value <<
      " due to parser error=" << parsingError;
    return true;
  }
  VLOG(5) << "Processing header=" << name << " value=" << value;
  folly::StringPiece nameSp(name);
  folly::StringPiece valueSp(value);

  if (nameSp.startsWith(':')) {
    if (regularHeaderSeen) {
      parsingError = folly::to<string>("Illegal pseudo header name=", nameSp);
      return false;
    }
    if (isRequest) {
      if (nameSp == http2::kMethod) {
        if (!verifier.setMethod(valueSp)) {
          return false;
        }
      } else if (nameSp == http2::kScheme) {
        if (!verifier.setScheme(valueSp)) {
          return false;
        }
      } else if (nameSp == http2::kAuthority) {
        if (!verifier.setAuthority(valueSp)) {
          return false;
        }
      } else if (nameSp == http2::kPath) {
        if (!verifier.setPath(valueSp)) {
          return false;
        }
      } else if (nameSp == http2::kProtocol) {
        if (!verifier.setUpgradeProtocol(valueSp)) {
          return false;
        }
      } else {
        parsingError = folly::to<string>("Invalid req header name=", nameSp);
        return false;
      }
    } else {
      if (nameSp == http2::kStatus) {
        if (hasStatus) {
          parsingError = string("Duplicate status");
          return false;
        }
        hasStatus = true;
        int32_t code = -1;
        try {
          code = folly::to<unsigned int>(valueSp);
        } catch (const std::range_error& ex) {
        }
        if (code >= 100 && code <= 999) {
          msg->setStatusCode(code);
          msg->setStatusMessage(
              HTTPMessage::getDefaultReason(code));
        } else {
          parsingError = folly::to<string>("Malformed status code=", valueSp);
          return false;
        }
      } else {
        parsingError = folly::to<string>("Invalid resp header name=", nameSp);
        return false;
      }
    }
  } else {
    regularHeaderSeen = true;
    if (nameSp == "connection") {
      parsingError = string("HTTP/2 Message with Connection header");
      return false;
    }
    if (nameSp == "content-length") {
      uint32_t cl = 0;
      try {
        cl = folly::to<uint32_t>(valueSp);
      } catch (const std::range_error& ex) {
      }
      if (hasContentLength &&
          contentLength != cl) {
        parsingError = string("Multiple content-length headers");
        return false;
      }
      hasContentLength = true;
      contentLength = cl;
    }
    bool nameOk = SPDYUtil::validateHeaderName(nameSp);
    bool valueOk = SPDYUtil::validateHeaderValue(valueSp, SPDYUtil::STRICT);
    if (!nameOk || !valueOk) {
      parsingError = folly::to<string>("Bad header value: name=",
                                       nameSp, " value=", valueSp);
      return false;
    }
    // Add the (name, value) pair to headers
    msg->getHeaders().add(nameSp, valueSp);
  }
  return true;
}

void HeaderDecodeInfo::onHeadersComplete(HTTPHeaderSize decodedSize) {
  HTTPHeaders& headers = msg->getHeaders();

  if (isRequest) {
    auto combinedCookie = headers.combine(HTTP_HEADER_COOKIE, "; ");
    if (!combinedCookie.empty()) {
      headers.set(HTTP_HEADER_COOKIE, combinedCookie);
    }
    if (!verifier.validate()) {
      parsingError = verifier.error;
      return;
    }
  } else if (!hasStatus) {
    parsingError = string("Malformed response, missing :status");
    return;
  }

  msg->setHTTPVersion(1, 1);
  msg->setIngressHeaderSize(decodedSize);
}

}
