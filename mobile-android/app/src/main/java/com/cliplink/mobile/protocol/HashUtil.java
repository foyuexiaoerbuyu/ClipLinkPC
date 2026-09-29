package com.cliplink.mobile.protocol;

import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;

/**
 * SHA-256 摘要工具（FIPS 180-4），与 PC 端 src/util/sha256.h 对齐：
 * 输出 64 位小写 hex，无 0x 前缀；输入统一按 UTF-8 编码（见需求 §17 / §79）。
 *
 * 用途：contentHash 计算、防同步循环、历史去重。
 */
public final class HashUtil {

    private HashUtil() {
    }

    /**
     * 计算字符串的 SHA-256 hex（小写）。
     *
     * @param input 输入文本（UTF-8 编码）
     * @return 64 位小写 hex
     * @throws IllegalStateException 平台不支持 SHA-256 时抛出（Android 必支持）
     */
    public static String sha256Hex(String input) {
        if (input == null) {
            input = "";
        }
        return sha256Hex(input.getBytes(StandardCharsets.UTF_8));
    }

    /**
     * 计算字节数组的 SHA-256 hex（小写）。
     */
    public static String sha256Hex(byte[] data) {
        try {
            MessageDigest digest = MessageDigest.getInstance("SHA-256");
            byte[] hash = digest.digest(data);
            StringBuilder sb = new StringBuilder(hash.length * 2);
            for (byte b : hash) {
                sb.append(Character.forDigit((b >> 4) & 0xF, 16));
                sb.append(Character.forDigit(b & 0xF, 16));
            }
            return sb.toString();
        } catch (NoSuchAlgorithmException e) {
            throw new IllegalStateException("SHA-256 not available", e);
        }
    }
}
