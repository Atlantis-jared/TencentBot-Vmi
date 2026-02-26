#include "SharedDataStatus.h"

SharedDataStatus g_shared_data{};

SharedDataSnapshot read_shared_data_snapshot() {
    SharedDataSnapshot snap{};
    snap.sync_flag = g_shared_data.sync_flag;
    snap.timestamp = g_shared_data.timestamp;
    snap.current_x = g_shared_data.current_x;
    snap.current_y = g_shared_data.current_y;
    snap.map_id = g_shared_data.map_id;
    return snap;
}

bool is_shared_data_ready() {
    return g_shared_data.sync_flag == 1;
}

