#
#  Copyright (c) 2026, edk2-flange contributors.
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
#

  # Device Tree Support
  INF RuleOverride = DTB $(PLATFORM_DIRECTORY)/DeviceTree/Mainline.inf

  # LT9611 control lines
  INF $(PLATFORM_DIRECTORY)/Drivers/BoardDxe/BoardDxe.inf
