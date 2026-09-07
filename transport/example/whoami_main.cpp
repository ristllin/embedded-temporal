// transport example: live whoami against the real Mistral control plane.
// A live proof for the REST client — prints the real Temporal scheduler
// endpoint + namespace discovered for the API key.
//
//   export MISTRAL_API_KEY=...      # from a local .env file — NEVER hardcode
//   ./whoami
//
// Expected live result: scheduler_url = wf-scheduler.mistral.ai:443, tls = true.
#include <cstdlib>
#include <iostream>

#include "rest_client.h"

int main(int argc, char** argv) {
  const char* key = std::getenv("MISTRAL_API_KEY");
  if (!key || !*key) {
    std::cerr << "MISTRAL_API_KEY not set (source a local .env file)\n";
    return 2;
  }
  const std::string base =
      argc > 1 ? argv[1] : "https://api.mistral.ai";

  mwf_transport::RestClient client(base, key);

  std::cout << "[whoami] GET " << base << "/v1/workflows/workers/whoami\n";
  auto r = client.whoami();
  if (!r) {
    std::cerr << "[whoami] FAILED: " << r.error << "\n";
    return 1;
  }

  const mwf_transport::WhoAmI& w = r.value;
  std::cout << "[whoami] OK\n"
            << "  scheduler_url = " << w.scheduler_url << "\n"
            << "  namespace     = " << w.namespace_ << "\n"
            << "  tls           = " << (w.tls ? "true" : "false") << "\n"
            << "  raw           = " << w.raw << "\n";

  // The scheduler_url is exactly the DesktopTransport target for the gRPC data
  // plane, and namespace is the temporal-namespace metadata value.
  return 0;
}
