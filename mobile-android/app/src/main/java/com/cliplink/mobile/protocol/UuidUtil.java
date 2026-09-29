package com.cliplink.mobile.protocol;

import java.util.UUID;

/**
 * UUID v4 生成，与 PC 端 src/util/uuid.h 对齐（见需求 §18 eventId / §19 deviceId）：
 * 小写标准格式，例如 "550e8400-e29b-41d4-a716-446655440000"。
 */
public final class UuidUtil {

    private UuidUtil() {
    }

    /**
     * 生成一个随机 UUID v4（小写标准格式）。
     */
    public static String uuidV4() {
        return UUID.randomUUID().toString();
    }
}
