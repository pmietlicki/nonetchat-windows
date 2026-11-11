#include <windows.h>
#include <netlistmgr.h>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Networking.Connectivity.h>

#include <algorithm>
#include <chrono>
#include <cstring>

// Read only: never scan BSSIDs, request location, read a Wi-Fi password or
// change a network. Microsoft recommends GetConnectedSsid rather than the
// location-gated wlan_intf_opcode_current_connection. The caller supplies the
// exact WLAN interface GUID, not a cached profile or the Internet adapter.
extern "C" __declspec(dllexport) unsigned long nonet_wifi_connection(
    const unsigned char* interface_id, unsigned int timeout_ms, wchar_t* profile,
    unsigned int profile_capacity, wchar_t* ssid, unsigned int ssid_capacity) {
  if (!interface_id || !profile || !ssid || profile_capacity < 256 ||
      ssid_capacity < 33 || timeout_ms == 0) {
    return ERROR_INVALID_PARAMETER;
  }
  profile[0] = L'\0';
  ssid[0] = L'\0';
  try {
    // This export runs on its own short-lived Dart worker thread. Balance COM
    // initialization after all WinRT objects (including a cancelled query) die.
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    struct Apartment final {
      ~Apartment() { winrt::uninit_apartment(); }
    } apartment;
    using namespace winrt::Windows::Networking::Connectivity;
    // Do not retain a cached activation factory beyond this balanced COM
    // apartment: repeated short-lived worker calls must not reuse a retired
    // factory. This is the public uncached WinRT factory overload.
    const auto information =
        winrt::try_get_activation_factory<INetworkInformationStatics>(
            winrt::name_of<NetworkInformation>());
    if (!information) return ERROR_NOT_SUPPORTED;
    for (const auto& candidate : information.GetConnectionProfiles()) {
      const auto adapter = candidate.NetworkAdapter();
      if (!adapter) continue;
      const auto id = adapter.NetworkAdapterId();
      if (std::memcmp(&id, interface_id, sizeof(id)) != 0) continue;
      auto pending = adapter.GetConnectedProfileAsync();
      if (pending.wait_for(std::chrono::milliseconds(timeout_ms)) !=
          winrt::Windows::Foundation::AsyncStatus::Completed) {
        pending.Cancel();
        return ERROR_TIMEOUT;
      }
      const auto current = pending.GetResults();
      if (!current || !current.IsWlanConnectionProfile()) {
        return ERROR_NOT_CONNECTED;
      }
      const auto name = current.ProfileName();
      const auto network =
          current.WlanConnectionProfileDetails().GetConnectedSsid();
      // Refuse truncation: ownership/restoration checks need exact values.
      if (name.empty() || name.size() >= profile_capacity || network.empty() ||
          network.size() >= ssid_capacity) {
        return ERROR_INVALID_DATA;
      }
      std::copy(name.begin(), name.end(), profile);
      profile[name.size()] = L'\0';
      std::copy(network.begin(), network.end(), ssid);
      ssid[network.size()] = L'\0';
      return ERROR_SUCCESS;
    }
    return ERROR_NOT_FOUND;
  } catch (const winrt::hresult_error& error) {
    return error.code() == E_ACCESSDENIED ? ERROR_ACCESS_DENIED
                                         : ERROR_NOT_SUPPORTED;
  } catch (...) {
    return ERROR_NOT_SUPPORTED;
  }
}

// Check only the connected network on the exact WLAN adapter. Category changes
// require Administrator membership (INetwork::SetCategory); never elevate or
// modify a user's network/firewall. No profile names or addresses leave here.
extern "C" __declspec(dllexport) unsigned long nonet_wifi_public_network(
    const unsigned char* interface_id, int* is_public) {
  if (!interface_id || !is_public) return ERROR_INVALID_PARAMETER;
  *is_public = 0;
  try {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    struct Apartment final {
      ~Apartment() { winrt::uninit_apartment(); }
    } apartment;
    winrt::com_ptr<INetworkListManager> manager;
    winrt::check_hresult(::CoCreateInstance(
        __uuidof(NetworkListManager), nullptr, CLSCTX_ALL,
        __uuidof(INetworkListManager), manager.put_void()));
    winrt::com_ptr<IEnumNetworkConnections> connections;
    winrt::check_hresult(manager->GetNetworkConnections(connections.put()));
    bool found = false;
    while (true) {
      winrt::com_ptr<INetworkConnection> connection;
      ULONG fetched = 0;
      const auto next = connections->Next(1, connection.put(), &fetched);
      winrt::check_hresult(next);
      if (next == S_FALSE || fetched == 0) break;
      GUID adapter{};
      winrt::check_hresult(connection->GetAdapterId(&adapter));
      if (std::memcmp(&adapter, interface_id, sizeof(adapter)) != 0) continue;
      VARIANT_BOOL connected = VARIANT_FALSE;
      winrt::check_hresult(connection->get_IsConnected(&connected));
      if (connected != VARIANT_TRUE) continue;
      winrt::com_ptr<INetwork> network;
      winrt::check_hresult(connection->GetNetwork(network.put()));
      NLM_NETWORK_CATEGORY category{};
      winrt::check_hresult(network->GetCategory(&category));
      if (category != NLM_NETWORK_CATEGORY_PUBLIC) return ERROR_SUCCESS;
      found = true;
    }
    if (!found) return ERROR_NOT_FOUND;
    *is_public = 1;
    return ERROR_SUCCESS;
  } catch (const winrt::hresult_error& error) {
    return error.code() == E_ACCESSDENIED ? ERROR_ACCESS_DENIED
                                        : ERROR_NOT_SUPPORTED;
  } catch (...) {
    return ERROR_NOT_SUPPORTED;
  }
}
