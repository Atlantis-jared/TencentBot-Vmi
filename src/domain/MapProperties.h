#pragma once

// 地图 UI 坐标换算参数：
// - uiPixelWidth/uiPixelHeight: 小地图在屏幕上的模板像素尺寸
// - gameCoordMaxX/gameCoordMaxY: 该地图游戏坐标的最大边界
struct MapProperties {
    int uiPixelWidth = 0;
    int uiPixelHeight = 0;
    int gameCoordMaxX = 0;
    int gameCoordMaxY = 0;
};
