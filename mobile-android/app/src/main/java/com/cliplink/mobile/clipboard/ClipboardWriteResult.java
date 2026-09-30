package com.cliplink.mobile.clipboard;

/**
 * 程序写入系统剪贴板的明确结果（见《PC 远程剪贴板通知》spec §4）。
 *
 * <p>旧实现只以"setPrimaryClip() 未抛异常"判定成功，无法区分
 * 写入被系统拒绝、内容被其它应用抢占等情况；本枚举配合
 * {@link ClipboardHelper#writeProgrammatic(String)} 的回读校验，
 * 让调用方（SyncManager）能据此选择通知形态：
 * SUCCESS -> 普通「已复制」通知；FAILED / NOT_ALLOWED -> 带「复制」按钮的通知。
 */
public enum ClipboardWriteResult {

    /** 写入成功：回读 ClipboardManager 当前内容与写入内容一致 */
    SUCCESS,

    /** 写入失败：异常、内容过大、回读内容不一致或回读为空且剪贴板为空 */
    FAILED,

    /** 系统不允许写入：SecurityException / 剪贴板服务不可用等被平台拒绝的情形 */
    NOT_ALLOWED
}
