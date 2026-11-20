#pragma once
// Q2 desktop/Linux hardware geometry adapter for CrossPoint's viewable
// content insets. The Q2 375x320 logical page has no e-paper bezel mask.
struct BoardConfig {
  struct ViewableInsets {
    int top=0, right=0, bottom=0, left=0;
  };
  struct DeviceConfig {
    ViewableInsets viewableInsets{};
  };
  static const DeviceConfig ACTIVE;
};
inline const BoardConfig::DeviceConfig BoardConfig::ACTIVE{};
