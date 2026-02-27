#include "SharedDataStatus.h"

SharedDataStatus g_shared_data{};

SharedDataSnapshot read_shared_data_snapshot() {
    SharedDataSnapshot snap{};
    snap.sync_flag = g_shared_data.sync_flag;
    snap.timestamp = g_shared_data.timestamp;
    snap.pit_x = g_shared_data.pit_x;
    snap.pit_y = g_shared_data.pit_y;
    snap.map_id = g_shared_data.map_id;
    snap.role_raw_x = g_shared_data.role_raw_x;
    snap.role_raw_y = g_shared_data.role_raw_y;
    return snap;
}

bool is_shared_data_ready() {
    return g_shared_data.sync_flag == 1;
}

