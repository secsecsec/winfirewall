/*
 * Connector API between Go and Windows Firewall COM interface
 * Windows XP API version
 */

const char const *program_suffix = _T(" (program rule)");
const char const *port_tcp_suffix = _T(" (port TCP rule)");
const char const *port_udp_suffix = _T(" (port UDP rule)");


// Initialize the Firewall COM service. This API doesn't require elevating
// privileges to operate on the Firewall.
HRESULT windows_firewall_initialize_compat_xp(OUT INetFwPolicy **policy)
{
    HRESULT hr = S_OK;
    HRESULT com_init = E_FAIL;
    INetFwMgr *fw_mgr = NULL;

    _ASSERT(policy != NULL);

    // Initialize COM.
    com_init = CoInitializeEx(0, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    // Ignore RPC_E_CHANGED_MODE; this just means that COM has already been
    // initialized with a different mode. Since we don't care what the mode is,
    // we'll just use the existing mode.
    if (com_init != RPC_E_CHANGED_MODE) {
        if (FAILED(com_init)) {
            return com_init;
        }
    }

    // Create an instance of the firewall settings manager.
    hr = CoCreateInstance(&CLSID_NetFwMgr,
                          NULL,
                          CLSCTX_INPROC_SERVER,
                          &IID_INetFwMgr,
                          (void**)&fw_mgr);
    GOTO_IF_FAILED(cleanup, hr);

    // Retrieve the local firewall policy.
    hr = INetFwMgr_get_LocalPolicy(fw_mgr, policy);
    GOTO_IF_FAILED(cleanup, hr);

cleanup:
    // Release the firewall settings manager.
    if (fw_mgr != NULL) {
        INetFwMgr_Release(fw_mgr);
    }

    return hr;
}

// Clean up the Firewall service safely
void windows_firewall_cleanup_compat_xp(IN INetFwPolicy *policy)
{
    if (policy != NULL) {
        INetFwPolicy_Release(policy);
        CoUninitialize();
    }
}

// Get Firewall status: returns a boolean for ON/OFF
HRESULT windows_firewall_is_on_compat_xp(IN INetFwPolicy *policy, OUT BOOL *fw_on)
{
    HRESULT hr = S_OK;
    VARIANT_BOOL fw_enabled;
    INetFwProfile *fw_profile;

    _ASSERT(policy != NULL);
    _ASSERT(fw_on != NULL);

    *fw_on = FALSE;

    // Retrieve the firewall profile currently in effect.
    GOTO_IF_FAILED(cleanup,
                   INetFwPolicy_get_CurrentProfile(policy, &fw_profile));

    // Get the current state of the firewall.
    GOTO_IF_FAILED(cleanup,
                   INetFwProfile_get_FirewallEnabled(fw_profile, &fw_enabled));

    // Check to see if the firewall is on.
    if (fw_enabled != VARIANT_FALSE) {
        *fw_on = TRUE;
    }

cleanup:
    if (fw_profile != NULL) {
        INetFwProfile_Release(fw_profile);
    }
    return hr;
}

//  Turn Firewall ON
HRESULT windows_firewall_turn_on_compat_xp(IN INetFwPolicy *policy)
{
    HRESULT hr = S_OK;
    VARIANT_BOOL fw_enabled;
    INetFwProfile *fw_profile;

    _ASSERT(policy != NULL);

    // Retrieve the firewall profile currently in effect.
    GOTO_IF_FAILED(cleanup,
                   INetFwPolicy_get_CurrentProfile(policy, &fw_profile));

    // Get the current state of the firewall.
    GOTO_IF_FAILED(cleanup,
                   INetFwProfile_get_FirewallEnabled(fw_profile, &fw_enabled));

    // If it is, turn it on.
    if (fw_enabled == VARIANT_FALSE) {
        GOTO_IF_FAILED(cleanup,
                       INetFwProfile_put_FirewallEnabled(fw_profile, VARIANT_TRUE));
    }

cleanup:
    if (fw_profile != NULL) {
        INetFwProfile_Release(fw_profile);
    }
    return hr;
}

//  Turn Firewall OFF
HRESULT windows_firewall_turn_off_compat_xp(IN INetFwPolicy *policy)
{
    HRESULT hr = S_OK;
    VARIANT_BOOL fw_enabled;
    INetFwProfile *fw_profile;

    _ASSERT(policy != NULL);

    // Retrieve the firewall profile currently in effect.
    GOTO_IF_FAILED(cleanup,
                   INetFwPolicy_get_CurrentProfile(policy, &fw_profile));

    // Get the current state of the firewall.
    GOTO_IF_FAILED(cleanup,
                   INetFwProfile_get_FirewallEnabled(fw_profile, &fw_enabled));

    // If it is, turn it off.
    if (fw_enabled == VARIANT_TRUE) {
        GOTO_IF_FAILED(cleanup,
                       INetFwProfile_put_FirewallEnabled(fw_profile, VARIANT_FALSE));
    }

cleanup:
    if (fw_profile != NULL) {
        INetFwProfile_Release(fw_profile);
    }
    return hr;
}


// Set a Firewall rule. In Windows XP, Firewall rules don't exist, so we emulate
// them as follows:
// * The only rule parameters taken into account are: name, application and port
// * Application rules and port are orthogonal, ie. if the rule specifies a program
//   it will be set for all ports of this program; if the rule specifies a port it
//   will open the port for all apps. More fine-grained rules are not possible in
//   this version of Windows. Both rules can be set independently. A null parameter
//   won't set that rule.
// * These rules are then registered with a suffix (application, port TCP/UDP).
HRESULT windows_firewall_rule_set_compat_xp(IN INetFwPolicy *policy,
                                            firewall_rule_t *rule)
{
    HRESULT hr = S_OK;

    TCHAR program_rule_name[1024] = {0};
    TCHAR port_tcp_rule_name[1024] = {0};
    TCHAR port_udp_rule_name[1024] = {0};
    BSTR bstr_program_rule_name = NULL;
    BSTR bstr_port_tcp_rule_name = NULL;
    BSTR bstr_port_udp_rule_name = NULL;
    BSTR bstr_application = NULL;

    INetFwProfile *fw_profile;
    INetFwAuthorizedApplication* fw_app = NULL;
    INetFwAuthorizedApplications* fw_apps = NULL;
    INetFwOpenPort* fw_open_port_tcp = NULL;
    INetFwOpenPort* fw_open_port_udp = NULL;
    INetFwOpenPorts* fw_open_ports = NULL;

    _ASSERT(policy != NULL);

    // Retrieve the firewall profile currently in effect.
    GOTO_IF_FAILED(cleanup,
                   INetFwPolicy_get_CurrentProfile(policy, &fw_profile));

    // Emulate API2 rules by applying Application and Port
    if (rule->application != NULL && rule->application[0] != 0) {

#define STRSIZE_IN_BYTES(x)  (_countof(x) * sizeof(TCHAR))
#define STR_COPY(x, y) StringCbCopy(x, STRSIZE_IN_BYTES(x), y);
#define STR_CAT(x, y) StringCbCat(x, STRSIZE_IN_BYTES(x), y);
        
        // Note: Windows won't register the rule if it's already registered

        STR_COPY(program_rule_name, rule->name);
        STR_CAT(program_rule_name, program_suffix);
        bstr_program_rule_name = chars_to_BSTR(program_rule_name);

        // Retrieve the authorized application collection
        GOTO_IF_FAILED(
            cleanup,
            INetFwProfile_get_AuthorizedApplications(fw_profile, &fw_apps)
            );
        // Create an instance of an authorized application
        GOTO_IF_FAILED(
            cleanup,
            CoCreateInstance(&CLSID_NetFwAuthorizedApplication,
                             NULL,
                             CLSCTX_INPROC_SERVER,
                             &IID_INetFwAuthorizedApplication,
                             (void**)&fw_app)
            );

        bstr_application = chars_to_BSTR(rule->application);

        GOTO_IF_FAILED(
            cleanup,
            INetFwAuthorizedApplication_put_ProcessImageFileName(
                fw_app,
                bstr_application
                )
            );
        GOTO_IF_FAILED(
            cleanup,
            INetFwAuthorizedApplication_put_Name(fw_app, bstr_program_rule_name)
            );

        GOTO_IF_FAILED(cleanup,
                       INetFwAuthorizedApplications_Add(fw_apps, fw_app));
    }

    if (rule->port != NULL && rule->port[0] != 0) {
        // Note: Windows won't register the rule if it's already registered

        // Retrieve the collection of globally open ports
        GOTO_IF_FAILED(
            cleanup,
            INetFwProfile_get_GloballyOpenPorts(fw_profile, &fw_open_ports)
            );

        // TCP Rule

        GOTO_IF_FAILED(
            cleanup,
            CoCreateInstance(&CLSID_NetFwOpenPort,
                             NULL,
                             CLSCTX_INPROC_SERVER,
                             &IID_INetFwOpenPort,
                             (void**)&fw_open_port_tcp)
            );

        STR_COPY(port_tcp_rule_name, rule->name);
        STR_CAT(port_tcp_rule_name, port_tcp_suffix);
        bstr_port_tcp_rule_name = chars_to_BSTR(port_tcp_rule_name);

        GOTO_IF_FAILED(
            cleanup,
            INetFwOpenPort_put_Port(fw_open_port_tcp, atoi(rule->port))
            );
        GOTO_IF_FAILED(
            cleanup,
            INetFwOpenPort_put_Protocol(fw_open_port_tcp, NET_FW_IP_PROTOCOL_TCP);
            );
        GOTO_IF_FAILED(
            cleanup,
            INetFwOpenPort_put_Name(fw_open_port_tcp, bstr_port_tcp_rule_name)
            );
        GOTO_IF_FAILED(
            cleanup,
            INetFwOpenPorts_Add(fw_open_ports, fw_open_port_tcp)
            );

        // UDP Rule
        GOTO_IF_FAILED(
            cleanup,
            CoCreateInstance(&CLSID_NetFwOpenPort,
                             NULL,
                             CLSCTX_INPROC_SERVER,
                             &IID_INetFwOpenPort,
                             (void**)&fw_open_port_udp)
            );

        STR_COPY(port_udp_rule_name, rule->name);
        STR_CAT(port_udp_rule_name, port_udp_suffix);
        bstr_port_udp_rule_name = chars_to_BSTR(port_udp_rule_name);

        GOTO_IF_FAILED(
            cleanup,
            INetFwOpenPort_put_Port(fw_open_port_udp, atoi(rule->port))
            );
        GOTO_IF_FAILED(
            cleanup,
            INetFwOpenPort_put_Protocol(fw_open_port_udp, NET_FW_IP_PROTOCOL_UDP);
            );
        GOTO_IF_FAILED(
            cleanup,
            INetFwOpenPort_put_Name(fw_open_port_udp, bstr_port_udp_rule_name)
            );
        GOTO_IF_FAILED(
            cleanup,
            INetFwOpenPorts_Add(fw_open_ports, fw_open_port_udp)
            );
    }

cleanup:
    if (fw_profile != NULL)
        INetFwProfile_Release(fw_profile);
    if (fw_app != NULL)
        INetFwAuthorizedApplication_Release(fw_app);
    if (fw_apps != NULL)
        INetFwAuthorizedApplications_Release(fw_apps);
    if (fw_open_port_tcp != NULL)
        INetFwOpenPort_Release(fw_open_port_tcp);
    if (fw_open_port_udp != NULL)
        INetFwOpenPort_Release(fw_open_port_udp);
    if (fw_open_ports != NULL)
        INetFwOpenPorts_Release(fw_open_ports);

    SysFreeString(bstr_program_rule_name);
    SysFreeString(bstr_port_tcp_rule_name);
    SysFreeString(bstr_port_udp_rule_name);
    SysFreeString(bstr_application);

    return hr;
}

// Test whether a Firewall rule exists or not. Since we are emulating API2,
// it will set {exists} to TRUE if any part of the rule exists (program or ports
// TCP/UDP).
HRESULT windows_firewall_rule_exists_compat_xp(IN INetFwPolicy *policy,
                                               IN firewall_rule_t *rule,
                                               OUT BOOL *exists)
{
    HRESULT hr = S_OK;

    INetFwProfile *fw_profile;
    INetFwAuthorizedApplication* fw_app = NULL;
    INetFwAuthorizedApplications* fw_apps = NULL;
    INetFwOpenPort* fw_open_port = NULL;
    INetFwOpenPorts* fw_open_ports = NULL;

    BSTR bstr_application = chars_to_BSTR(rule->application);

    *exists = FALSE;

    _ASSERT(policy != NULL);

    // Retrieve the firewall profile currently in effect.
    GOTO_IF_FAILED(cleanup,
                   INetFwPolicy_get_CurrentProfile(policy, &fw_profile));

    if (rule->application != NULL && rule->application[0] != 0) {
        // Retrieve the authorized application collection
        GOTO_IF_FAILED(
            cleanup,
            INetFwProfile_get_AuthorizedApplications(fw_profile, &fw_apps)
            );

        hr = INetFwAuthorizedApplications_Item(fw_apps, bstr_application, &fw_app);
        if (SUCCEEDED(hr)) {
            *exists = TRUE;
            goto cleanup;
        }
    }

    if (rule->port != NULL && rule->port[0] != 0) {
        // Retrieve the collection of globally open ports
        GOTO_IF_FAILED(
            cleanup,
            INetFwProfile_get_GloballyOpenPorts(fw_profile, &fw_open_ports)
            );

        hr = INetFwOpenPorts_Item(fw_open_ports, atoi(rule->port), NET_FW_IP_PROTOCOL_TCP, &fw_open_port);
        if (SUCCEEDED(hr)) {
            *exists = TRUE;
            goto cleanup;
        }

        hr = INetFwOpenPorts_Item(fw_open_ports, atoi(rule->port), NET_FW_IP_PROTOCOL_UDP, &fw_open_port);
        if (SUCCEEDED(hr)) {
            *exists = TRUE;
            goto cleanup;
        }
    }

cleanup:
    if (fw_profile != NULL)
        INetFwProfile_Release(fw_profile);
    if (fw_app != NULL)
        INetFwAuthorizedApplication_Release(fw_app);
    if (fw_apps != NULL)
        INetFwAuthorizedApplications_Release(fw_apps);
    if (fw_open_port != NULL)
        INetFwOpenPort_Release(fw_open_port);
    if (fw_open_ports != NULL)
        INetFwOpenPorts_Release(fw_open_ports);

    SysFreeString(bstr_application);

    return S_OK;
}

// Remove a Firewall rule if exists. IMPORTANT: Because we are emulating API2
// rules, only the components provided in the rulewill be removed (program
// and/or port TCP/UDP).
HRESULT windows_firewall_rule_remove_compat_xp(IN INetFwPolicy *policy,
                                               IN firewall_rule_t *rule)
{
    HRESULT hr = S_OK;

    INetFwProfile *fw_profile;
    INetFwAuthorizedApplication* fw_app = NULL;
    INetFwAuthorizedApplications* fw_apps = NULL;
    INetFwOpenPort* fw_open_port = NULL;
    INetFwOpenPorts* fw_open_ports = NULL;

    BSTR bstr_application = chars_to_BSTR(rule->application);

    _ASSERT(policy != NULL);

    // Retrieve the firewall profile currently in effect.
    GOTO_IF_FAILED(cleanup,
                   INetFwPolicy_get_CurrentProfile(policy, &fw_profile));

    if (rule->application != NULL && rule->application[0] != 0) {
        // Retrieve the authorized application collection
        GOTO_IF_FAILED(
            cleanup,
            INetFwProfile_get_AuthorizedApplications(fw_profile, &fw_apps)
            );

        hr = INetFwAuthorizedApplications_Item(fw_apps, bstr_application, &fw_app);
        if (SUCCEEDED(hr)) {
            GOTO_IF_FAILED(cleanup,
                           INetFwAuthorizedApplications_Remove(fw_apps, bstr_application));
        }
    }

    if (rule->port != NULL && rule->port[0] != 0) {
        // Retrieve the collection of globally open ports
        GOTO_IF_FAILED(
            cleanup,
            INetFwProfile_get_GloballyOpenPorts(fw_profile, &fw_open_ports)
            );

        hr = INetFwOpenPorts_Item(fw_open_ports, atoi(rule->port), NET_FW_IP_PROTOCOL_TCP, &fw_open_port);
        if (SUCCEEDED(hr)) {
            GOTO_IF_FAILED(
                cleanup,
                INetFwOpenPorts_Remove(
                    fw_open_ports,
                    atoi(rule->port),
                    NET_FW_IP_PROTOCOL_TCP
                    )
                );
        }

        hr = INetFwOpenPorts_Item(fw_open_ports, atoi(rule->port), NET_FW_IP_PROTOCOL_UDP, &fw_open_port);
        if (SUCCEEDED(hr)) {
            GOTO_IF_FAILED(
                cleanup,
                INetFwOpenPorts_Remove(
                    fw_open_ports,
                    atoi(rule->port),
                    NET_FW_IP_PROTOCOL_UDP
                    )
                );
        }
    }

cleanup:
    if (fw_profile != NULL)
        INetFwProfile_Release(fw_profile);
    if (fw_app != NULL)
        INetFwAuthorizedApplication_Release(fw_app);
    if (fw_apps != NULL)
        INetFwAuthorizedApplications_Release(fw_apps);
    if (fw_open_port != NULL)
        INetFwOpenPort_Release(fw_open_port);
    if (fw_open_ports != NULL)
        INetFwOpenPorts_Release(fw_open_ports);

    SysFreeString(bstr_application);

    return S_OK;
}
