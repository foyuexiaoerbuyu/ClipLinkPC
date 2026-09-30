package com.cliplink.mobile.clipboard;

/**
 * 程序写入系统剪贴板的明确结果（见《PC 远程剪贴板通知》spec §4）。
 *
 * <p>旧实现只以"setPrimaryClip() 未抛异常"判定成功，无法区分
 * 写入被系统拒绝、内容被其它应用抢占等情况；本枚举配合
 * {@link ClipboardHelper#writeProgrammatic(String)} 的回读校验，
 * 让调用方（SyncManager / 通知层）能据此选择通知形态：
 * SUCCESS -> 普通「已复制」通知；FAILED / NOT_ALLOWED -> 带「复制」按钮的通知；
 * UNVERIFIED -> 无法校验（后台焦点限制），默认按保守策略走带「复制」按钮的通知。
 *
 * <p>三态语义（真机 Android 14 实测修正）：
 * <ul>
 *   <li>{@link #SUCCESS}：回读内容与写入内容一致（写入确实生效）；</li>
 *   <li>{@link #UNVERIFIED}：写入调用未抛异常、已被系统接受，但系统在后台
 *       禁止本应用回读剪贴板（焦点限制），<b>既不能证明成功也不能证明失败</b>。
 *       它不是失败——Android 10+ 后台应用回读剪贴板会被系统伪装成"空剪贴板"，
 *       若把它当成 FAILED 会误报"自动复制失败"（真机实测：OPPO 自动填充服务
 *       AutoFillLog.AIUnitHelper 仍在 1 秒内检测到剪贴板变化，证明写入已生效）；</li>
 *   <li>{@link #FAILED}：<b>确定失败</b>——写入抛异常、内容过大/为空、
 *       回读到的内容与写入内容不一致，或应用在前台仍读不到写入内容。</li>
 * </ul>
 */
public enum ClipboardWriteResult {

    /** 写入成功：回读 ClipboardManager 当前内容与写入内容一致 */
    SUCCESS,

    /**
     * 无法校验：写入调用已被系统接受（未抛异常），但系统拒绝后台应用回读剪贴板，
     * 既不能证明成功也不能证明失败。与 {@link #FAILED}（确定失败）严格区分：
     * 自动路径默认仍展示带「复制」按钮的通知（保守），
     * 用户点击「复制」时按"已复制"更新通知（用户明确意图）。
     */
    UNVERIFIED,

    /** 写入失败：异常、内容过大、回读内容不一致或前台回读为空 */
    FAILED,

    /** 系统不允许写入：SecurityException / 剪贴板服务不可用等被平台拒绝的情形 */
    NOT_ALLOWED
}
