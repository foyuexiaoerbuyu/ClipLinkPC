package com.cliplink.mobile.clipboard;

/**
 * 剪贴板事件来源分类（见需求 §12-§13，与 PC 端 ClipboardSource 一致）。
 *
 * USER         - 用户真实复制：保存历史 + 发送服务器
 * REMOTE       - 服务器同步来的：写入本地剪贴板 + 入库，不再次发送服务器
 * HISTORY      - 点击历史项产生：写入剪贴板，不入库、不发送服务器（§83）
 * NOTIFICATION - 点击远程剪贴板通知里的「复制」产生：写入剪贴板，
 *                不入库、不发送服务器、不生成 eventId、不广播
 *                （见《PC 远程剪贴板通知》spec §3 / §9 防回环）
 *
 * <p>NOTIFICATION 与 REMOTE、HISTORY 一样：允许写入系统剪贴板，
 * 但严禁产生新的同步事件（不得成为新的同步来源）。
 */
public enum ClipboardSource {
    USER,
    REMOTE,
    HISTORY,
    NOTIFICATION
}
