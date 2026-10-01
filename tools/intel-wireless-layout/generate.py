#!/usr/bin/env python3
"""Writes two C files that export the same table of field offsets and sizes:
one computed from Linux's iwlwifi firmware API headers (dual GPL/BSD), one from
kernel/drivers/intel_wireless_api.h. compare.c decides whether they agree.
usage: generate.py <output directory>"""
import os, sys

# (key, Linux struct, Linux field, lean_os type, lean_os field); a field of None
# compares the whole structure's size.
ROWS = [
 ("command_header", "iwl_cmd_header_wide", None, "intel_wireless_command_header_t", None),
 ("command_header.sequence", "iwl_cmd_header_wide", "sequence", "intel_wireless_command_header_t", "sequence"),
 ("command_header.length", "iwl_cmd_header_wide", "length", "intel_wireless_command_header_t", "length"),
 ("command_header.version", "iwl_cmd_header_wide", "version", "intel_wireless_command_header_t", "version"),
 ("context_info", "iwl_context_info", None, "intel_wireless_context_info_t", None),
 ("context_info.control", "iwl_context_info", "control.control_flags", "intel_wireless_context_info_t", "control_flags"),
 ("context_info.free_rbd", "iwl_context_info", "rbd_cfg.free_rbd_addr", "intel_wireless_context_info_t", "free_rbd_address"),
 ("context_info.used_rbd", "iwl_context_info", "rbd_cfg.used_rbd_addr", "intel_wireless_context_info_t", "used_rbd_address"),
 ("context_info.status", "iwl_context_info", "rbd_cfg.status_wr_ptr", "intel_wireless_context_info_t", "status_write_pointer"),
 ("context_info.cmd_queue", "iwl_context_info", "hcmd_cfg.cmd_queue_addr", "intel_wireless_context_info_t", "command_queue_address"),
 ("context_info.cmd_queue_size", "iwl_context_info", "hcmd_cfg.cmd_queue_size", "intel_wireless_context_info_t", "command_queue_size"),
 ("context_info.umac", "iwl_context_info", "dram.umac_img", "intel_wireless_context_info_t", "umac_image"),
 ("context_info.lmac", "iwl_context_info", "dram.lmac_img", "intel_wireless_context_info_t", "lmac_image"),
 ("context_info.virtual", "iwl_context_info", "dram.virtual_img", "intel_wireless_context_info_t", "virtual_image"),
 ("alive_v6", "iwl_alive_ntf_v7", None, "intel_wireless_alive_v6_t", None),
 ("alive_v6.lmac1_major", "iwl_alive_ntf_v7", "lmac_data[1].ucode_major", "intel_wireless_alive_v6_t", "lmac[1].ucode_major"),
 ("alive_v6.lmac0_error", "iwl_alive_ntf_v7", "lmac_data[0].dbg_ptrs.error_event_table_ptr", "intel_wireless_alive_v6_t", "lmac[0].debug.error_event_table_pointer"),
 ("alive_v6.umac_error", "iwl_alive_ntf_v7", "umac_data.dbg_ptrs.error_info_addr", "intel_wireless_alive_v6_t", "umac.error_info_address"),
 ("alive_v6.sku", "iwl_alive_ntf_v7", "sku_id", "intel_wireless_alive_v6_t", "sku_id"),
 ("alive_v6.imr", "iwl_alive_ntf_v7", "imr.enabled", "intel_wireless_alive_v6_t", "imr_enabled"),
 ("nvm_rsp_v4", "iwl_nvm_get_info_rsp_v4", None, "intel_wireless_nvm_get_info_response_v4_t", None),
 ("nvm_rsp_v4.n_hw_addrs", "iwl_nvm_get_info_rsp_v4", "general.n_hw_addrs", "intel_wireless_nvm_get_info_response_v4_t", "hardware_addresses"),
 ("nvm_rsp_v4.mac_sku", "iwl_nvm_get_info_rsp_v4", "mac_sku.mac_sku_flags", "intel_wireless_nvm_get_info_response_v4_t", "mac_sku_flags"),
 ("nvm_rsp_v4.tx_chains", "iwl_nvm_get_info_rsp_v4", "phy_sku.tx_chains", "intel_wireless_nvm_get_info_response_v4_t", "tx_chains"),
 ("nvm_rsp_v4.lar", "iwl_nvm_get_info_rsp_v4", "regulatory.lar_enabled", "intel_wireless_nvm_get_info_response_v4_t", "lar_enabled"),
 ("nvm_rsp_v4.n_channels", "iwl_nvm_get_info_rsp_v4", "regulatory.n_channels", "intel_wireless_nvm_get_info_response_v4_t", "channel_count"),
 ("nvm_rsp_v4.profile", "iwl_nvm_get_info_rsp_v4", "regulatory.channel_profile", "intel_wireless_nvm_get_info_response_v4_t", "channel_profile"),
 ("mcc_update", "iwl_mcc_update_cmd", None, "intel_wireless_mcc_update_t", None),
 ("mcc_update.source", "iwl_mcc_update_cmd", "source_id", "intel_wireless_mcc_update_t", "source_id"),
 ("mcc_rsp_v4", "iwl_mcc_update_resp_v4", None, "intel_wireless_mcc_update_response_v4_t", None),
 ("mcc_rsp_v4.n_channels", "iwl_mcc_update_resp_v4", "n_channels", "intel_wireless_mcc_update_response_v4_t", "channel_count"),
 ("bt_coex", "iwl_bt_coex_cmd", None, "intel_wireless_bt_coex_t", None),
 ("soc", "iwl_soc_configuration_cmd", None, "intel_wireless_soc_configuration_t", None),
 ("error_resp", "iwl_error_resp", None, "intel_wireless_error_response_t", None),
 ("error_resp.timestamp", "iwl_error_resp", "timestamp", "intel_wireless_error_response_t", "timestamp"),
 ("mac_ctx", "iwl_mac_ctx_cmd", None, "intel_wireless_mac_context_t", None),
 ("mac_ctx.node", "iwl_mac_ctx_cmd", "node_addr", "intel_wireless_mac_context_t", "node_address"),
 ("mac_ctx.bssid", "iwl_mac_ctx_cmd", "bssid_addr", "intel_wireless_mac_context_t", "bssid_address"),
 ("mac_ctx.filter", "iwl_mac_ctx_cmd", "filter_flags", "intel_wireless_mac_context_t", "filter_flags"),
 ("mac_ctx.ac", "iwl_mac_ctx_cmd", "ac", "intel_wireless_mac_context_t", "ac"),
 ("mac_ctx.ac3_txop", "iwl_mac_ctx_cmd", "ac[3].edca_txop", "intel_wireless_mac_context_t", "ac[3].edca_txop"),
 ("mac_ctx.sta_assoc", "iwl_mac_ctx_cmd", "sta.is_assoc", "intel_wireless_mac_context_t", "station.is_assoc"),
 ("mac_ctx.sta_listen", "iwl_mac_ctx_cmd", "sta.listen_interval", "intel_wireless_mac_context_t", "station.listen_interval"),
 ("mac_ctx.sta_aid", "iwl_mac_ctx_cmd", "sta.assoc_id", "intel_wireless_mac_context_t", "station.assoc_id"),
 ("scan_config", "iwl_scan_config", None, "intel_wireless_scan_config_t", None),
 ("scan_config.rx", "iwl_scan_config", "rx_chains", "intel_wireless_scan_config_t", "rx_chains"),
 ("scan_req", "iwl_scan_req_umac_v17", None, "intel_wireless_scan_request_v17_t", None),
 ("scan_req.general", "iwl_scan_req_umac_v17", "scan_params.general_params", "intel_wireless_scan_request_v17_t", "general"),
 ("scan_req.gen_mac", "iwl_scan_req_umac_v17", "scan_params.general_params.scan_start_mac_or_link_id", "intel_wireless_scan_request_v17_t", "general.scan_start_mac_or_link_id"),
 ("scan_req.gen_budget", "iwl_scan_req_umac_v17", "scan_params.general_params.adwell_max_budget", "intel_wireless_scan_request_v17_t", "general.adwell_max_budget"),
 ("scan_req.gen_priority", "iwl_scan_req_umac_v17", "scan_params.general_params.scan_priority", "intel_wireless_scan_request_v17_t", "general.scan_priority"),
 ("scan_req.gen_passive", "iwl_scan_req_umac_v17", "scan_params.general_params.passive_dwell", "intel_wireless_scan_request_v17_t", "general.passive_dwell"),
 ("scan_req.channel", "iwl_scan_req_umac_v17", "scan_params.channel_params", "intel_wireless_scan_request_v17_t", "channel"),
 ("scan_req.ch1", "iwl_scan_req_umac_v17", "scan_params.channel_params.channel_config[1].channel_num", "intel_wireless_scan_request_v17_t", "channel.channels[1].channel_number"),
 ("scan_req.ch1_band", "iwl_scan_req_umac_v17", "scan_params.channel_params.channel_config[1].v2.band", "intel_wireless_scan_request_v17_t", "channel.channels[1].band"),
 ("scan_req.ch1_iter", "iwl_scan_req_umac_v17", "scan_params.channel_params.channel_config[1].v2.iter_count", "intel_wireless_scan_request_v17_t", "channel.channels[1].iteration_count"),
 ("scan_req.periodic", "iwl_scan_req_umac_v17", "scan_params.periodic_params", "intel_wireless_scan_request_v17_t", "periodic"),
 ("scan_req.probe", "iwl_scan_req_umac_v17", "scan_params.probe_params", "intel_wireless_scan_request_v17_t", "probe"),
 ("scan_req.direct", "iwl_scan_req_umac_v17", "scan_params.probe_params.direct_scan", "intel_wireless_scan_request_v17_t", "probe.direct_scan"),
 ("scan_req.bssids", "iwl_scan_req_umac_v17", "scan_params.probe_params.bssid_array", "intel_wireless_scan_request_v17_t", "probe.bssid_array"),
 ("scan_complete", "iwl_umac_scan_complete", None, "intel_wireless_scan_complete_t", None),
 ("scan_complete.status", "iwl_umac_scan_complete", "status", "intel_wireless_scan_complete_t", "status"),
 ("rx_mpdu.status", "iwl_rx_mpdu_desc", "status", "intel_wireless_rx_mpdu_v1_t", "status"),
 ("rx_mpdu.mflags2", "iwl_rx_mpdu_desc", "mac_flags2", "intel_wireless_rx_mpdu_v1_t", "mac_flags2"),
 ("rx_mpdu.energy_a", "iwl_rx_mpdu_desc", "v1.energy_a", "intel_wireless_rx_mpdu_v1_t", "energy_a"),
 ("rx_mpdu.channel", "iwl_rx_mpdu_desc", "v1.channel", "intel_wireless_rx_mpdu_v1_t", "channel"),
 ("rx_mpdu.rate", "iwl_rx_mpdu_desc", "v1.rate_n_flags", "intel_wireless_rx_mpdu_v1_t", "rate_and_flags"),
 ("rx_mpdu.tsf", "iwl_rx_mpdu_desc", "v1.tsf_on_air_rise", "intel_wireless_rx_mpdu_v1_t", "tsf_on_air_rise"),
]

LINUX_HEADERS = ["fw/api/cmdhdr.h", "fw/api/alive.h", "fw/api/nvm-reg.h", "fw/api/config.h", "fw/api/coex.h",
                 "fw/api/system.h", "fw/api/mac.h", "fw/api/scan.h", "fw/api/rx.h", "fw/api/debug.h",
                 "pcie/iwl-context-info.h"]


def side(includes, linux):
    name = "linux" if linux else "lean"
    out = ['#include "layout.h"'] + ['#include "%s"' % h for h in includes]
    out.append("const layout_row_t %s_rows[] = {" % name)
    for key, ls, lf, ns, nf in ROWS:
        t = ("struct " + ls) if linux else ns
        f = lf if linux else nf
        if f is None:
            out.append('    {"%s", 0, sizeof(%s)},' % (key, t))
        else:
            out.append('    {"%s", offsetof(%s, %s), sizeof(((%s *)0)->%s)},' % (key, t, f, t, f))
    out.append("};")
    out.append("const unsigned %s_row_count = sizeof(%s_rows) / sizeof(%s_rows[0]);" % (name, name, name))
    return "\n".join(out) + "\n"


out_dir = sys.argv[1]
os.makedirs(out_dir, exist_ok=True)
open(os.path.join(out_dir, "linux_side.c"), "w").write(side(["prelude.h"] + LINUX_HEADERS, True))
open(os.path.join(out_dir, "lean_side.c"), "w").write(side(["drivers/intel_wireless_api.h"], False))
