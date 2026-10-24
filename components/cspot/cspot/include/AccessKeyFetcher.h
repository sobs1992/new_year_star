#pragma once

#include <atomic>      // or std::atomic
#include <functional>  // for function
#include <memory>      // for shared_ptr
#include <string>      // for string

namespace cspot {
struct Context;

class AccessKeyFetcher {
 public:
  AccessKeyFetcher(std::shared_ptr<cspot::Context> ctx);

  /**
  * @brief Checks if key is expired
  * @returns true when currently held access key is not valid
  */
  bool isExpired();

  /**
  * @brief Fetches a new access key
  * @remark In case the key is expired, this function blocks until a refresh is done.
  * @returns access key
  */
  std::string getAccessKey();

  /**
  * @brief Forces a refresh of the access key
  */
  void updateAccessKey();

  /**
  * @brief Called after every token request: true when Spotify gave a token
  * (false: wrong client id / secret, or no connection)
  */
  static std::function<void(bool)> resultHandler;

 private:
  std::shared_ptr<cspot::Context> ctx;

  std::atomic<bool> keyPending = false;
  std::string accessKey;
  long long int expiresAt;
  long long int retryAt = 0;  // after a failed request, to not hammer the server
};
}  // namespace cspot
