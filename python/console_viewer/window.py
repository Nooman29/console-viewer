"""The window on screen, drawn with OpenCV."""

from __future__ import annotations

import cv2
import numpy as np


class Window:
    NAME = "Console Viewer"

    def __init__(self, width: int = 1280, height: int = 720) -> None:
        self._fallback = (width, height)
        cv2.namedWindow(self.NAME, cv2.WINDOW_NORMAL)
        cv2.resizeWindow(self.NAME, width, height)

    def size(self) -> tuple[int, int]:
        """The drawable area in pixels."""
        try:
            _, _, width, height = cv2.getWindowImageRect(self.NAME)
        except cv2.error:
            return self._fallback
        if width <= 0 or height <= 0:
            return self._fallback
        return width, height

    def show(self, image: np.ndarray) -> None:
        cv2.imshow(self.NAME, image)

    def poll_key(self, milliseconds: int) -> int:
        """Wait briefly for a key. Returns its code, or -1 if none was pressed."""
        key = cv2.waitKey(max(1, milliseconds))
        return -1 if key == -1 else key & 0xFF

    def set_fullscreen(self, on: bool) -> None:
        cv2.setWindowProperty(
            self.NAME, cv2.WND_PROP_FULLSCREEN,
            cv2.WINDOW_FULLSCREEN if on else cv2.WINDOW_NORMAL,
        )

    def is_open(self) -> bool:
        """False once the user has closed the window with its X button."""
        try:
            return cv2.getWindowProperty(self.NAME, cv2.WND_PROP_VISIBLE) >= 1
        except cv2.error:
            return False

    def close(self) -> None:
        try:
            cv2.destroyWindow(self.NAME)
        except cv2.error:
            pass
