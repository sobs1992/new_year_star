#include "AccessKeyFetcher.h"

#include <cstring>           // for strrchr
#include <initializer_list>  // for initializer_list
#include <map>               // for operator!=, operator==
#include <type_traits>       // for remove_extent_t
#include <vector>            // for vector

#include "BellLogger.h"    // for AbstractLogger
#include "BellUtils.h"     // for BELL_SLEEP_MS
#include "CSpotContext.h"  // for Context
#include "HTTPClient.h"
#include "Logger.h"            // for CSPOT_LOG
#include "MercurySession.h"    // for MercurySession, MercurySession::Res...
#include "NanoPBExtensions.h"  // for bell::nanopb::encode...
#include "NanoPBHelper.h"      // for pbEncode and pbDecode
#include "Packet.h"            // for cspot
#include "TimeProvider.h"      // for TimeProvider
#include "Utils.h"             // for string_format

#ifdef BELL_ONLY_CJSON
#include "cJSON.h"
#else
#include "nlohmann/json.hpp"      // for basic_json<>::object_t, basic_json
#include "nlohmann/json_fwd.hpp"  // for json
#endif

using namespace cspot;

static std::string SCOPES =
    "streaming,user-library-read,user-library-modify,user-top-read,user-read-"
    "recently-played";  // Required access scopes

AccessKeyFetcher::AccessKeyFetcher(std::shared_ptr<cspot::Context> ctx)
    : ctx(ctx) {}

bool AccessKeyFetcher::isExpired() {
  if (accessKey.empty()) {
    return true;
  }

  if (ctx->timeProvider->getSyncedTimestamp() > expiresAt) {
    return true;
  }

  return false;
}

std::string AccessKeyFetcher::getAccessKey() {
  if (!isExpired()) {
    return accessKey;
  }

  updateAccessKey();

  return accessKey;
}

std::function<void(bool)> AccessKeyFetcher::resultHandler = nullptr;

void AccessKeyFetcher::updateAccessKey() {
  if (keyPending) {
    // Already pending refresh request
    return;
  }

  // Wrong credentials stay wrong: retry a failed request only now and then
  if (ctx->timeProvider->getSyncedTimestamp() < retryAt) {
    return;
  }

  keyPending = true;

  // Max retry of 3, can receive different hash cat types
  int retryCount = 3;
  bool success = false;

  do {
    CSPOT_LOG(info, "Access token expired, fetching new one...");

    auto credentials =
        "grant_type=client_credentials&client_id=" + ctx->config.clientId +
        "&client_secret=" + ctx->config.clientSecret;
    std::vector<uint8_t> body(credentials.begin(), credentials.end());

    // A network failure must not escape: the track queue task has no handler
    std::string_view responseBody;
    std::unique_ptr<bell::HTTPClient::Response> response;
    try {
      response = bell::HTTPClient::post(
          "https://accounts.spotify.com/api/token",
          {{"Content-Type", "application/x-www-form-urlencoded"}}, body);
      responseBody = response->body();
    } catch (const std::exception& e) {
      CSPOT_LOG(error, "Access token request failed: %s", e.what());
    }

    cJSON* root = cJSON_ParseWithLength(responseBody.data(), responseBody.size());
    cJSON* token = cJSON_GetObjectItem(root, "access_token");
    cJSON* expires = cJSON_GetObjectItem(root, "expires_in");
    if (cJSON_IsString(token) && cJSON_IsNumber(expires)) {
      accessKey = std::string(token->valuestring);
      int expiresIn = expires->valueint;

      // Successfully received an auth token
      CSPOT_LOG(info, "Access token sucessfully fetched");
      success = true;

      this->expiresAt =
          ctx->timeProvider->getSyncedTimestamp() + (expiresIn * 1000);
    } else {
      cJSON* error = cJSON_GetObjectItem(root, "error");
      CSPOT_LOG(error, "Failed to fetch access token: %s",
                cJSON_IsString(error) ? error->valuestring : "no answer");
      if (cJSON_IsString(error)) {
        retryCount = 0;  // the server said no: retrying right away won't help
      } else {
        BELL_SLEEP_MS(3000);
      }
    }
    cJSON_Delete(root);

    retryCount--;
  } while (retryCount >= 0 && !success);

  if (!success) {
    retryAt = ctx->timeProvider->getSyncedTimestamp() + 30000;
  }
  if (resultHandler) {
    resultHandler(success);
  }

  keyPending = false;
}
