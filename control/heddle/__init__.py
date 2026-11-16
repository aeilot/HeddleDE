"""Heddle 控制端：连接合成器控制套接字，按 agent 截图、注入输入、读窗口位置。"""
from .client import Heddle, HeddleError, default_control_path

__all__ = ["Heddle", "HeddleError", "default_control_path"]
