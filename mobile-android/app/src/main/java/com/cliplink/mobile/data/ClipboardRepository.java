package com.cliplink.mobile.data;

import android.content.ContentValues;
import android.content.Context;
import android.database.Cursor;
import android.database.sqlite.SQLiteDatabase;

import com.cliplink.mobile.protocol.ClipboardEvent;

import java.util.ArrayList;
import java.util.List;

/**
 * 剪贴板历史仓库（见需求 §25 / §28-§29 / §44 / §85-§87），逻辑与 PC 端
 * src/database/ClipboardRepository 对齐：
 *
 * - 幂等插入：event_id UNIQUE，重复 eventId 不产生新行（§87）
 * - 相邻去重：与最近一条 contentHash 相同则跳过（§29，非全表唯一）
 * - created_at DESC 分页查询（§75）
 * - 按 maxHistoryCount / maxHistoryDays 清理，满足任意条件即清理（§28）
 * - serverSeq / sync_status 更新与待同步查询（§44 / §85 / §86）
 * - app_state 读写 lastServerSeq 游标（§48）
 */
public class ClipboardRepository {

    /** 插入结果（与 PC 端 InsertOutcome 一致） */
    public enum InsertOutcome {
        INSERTED,            // 新事件已入库
        DUPLICATE_EVENT_ID,  // eventId 已存在（幂等命中，不重复插入）
        ADJACENT_DUPLICATE,  // 与最近一条 contentHash 相同，跳过（§29）
        FAILED               // 数据库执行失败
    }

    private static final long MILLIS_PER_DAY = 24L * 60L * 60L * 1000L;

    private static final String[] SELECT_COLUMNS = {
            "event_id", "device_id", "content_hash", "content_type",
            "content", "client_time", "server_time", "server_seq",
            "created_at", "sync_status"
    };

    private final DatabaseHelper dbHelper;

    public ClipboardRepository(Context context) {
        this.dbHelper = new DatabaseHelper(context.getApplicationContext());
    }

    /**
     * 插入事件：eventId 幂等优先，其次相邻去重；
     * createdAt &lt;= 0 时取当前 UTC 毫秒（§80）。
     */
    public synchronized InsertOutcome insertEvent(ClipboardEvent event) {
        if (event == null) {
            return InsertOutcome.FAILED;
        }
        if (event.getCreatedAt() <= 0) {
            event.setCreatedAt(System.currentTimeMillis());
        }

        SQLiteDatabase db = dbHelper.getWritableDatabase();
        db.beginTransaction();
        try {
            // 1. eventId 幂等：已存在则不重复插入（§87 / §44）
            try (Cursor c = db.query("clipboard_history",
                    new String[]{"event_id"},
                    "event_id = ?",
                    new String[]{event.getEventId()},
                    null, null, null, "1")) {
                if (c.moveToFirst()) {
                    db.setTransactionSuccessful();
                    return InsertOutcome.DUPLICATE_EVENT_ID;
                }
            }

            // 2. 相邻最近一次 contentHash 相同则跳过（§29，非全表唯一）
            try (Cursor c = db.query("clipboard_history",
                    new String[]{"content_hash"},
                    null, null, null, null,
                    "id DESC", "1")) {
                if (c.moveToFirst()
                        && event.getContentHash().equals(c.getString(0))) {
                    db.setTransactionSuccessful();
                    return InsertOutcome.ADJACENT_DUPLICATE;
                }
            }

            // 3. 插入；server_seq / server_time 未分配时存 NULL（§86）
            ContentValues values = new ContentValues();
            values.put("event_id", event.getEventId());
            values.put("device_id", event.getDeviceId());
            values.put("content_hash", event.getContentHash());
            values.put("content_type", event.getContentType());
            values.put("content", event.getContent());
            values.put("client_time", event.getClientTime());
            if (event.getServerTime() > 0) {
                values.put("server_time", event.getServerTime());
            } else {
                values.putNull("server_time");
            }
            if (event.getServerSeq() > 0) {
                values.put("server_seq", event.getServerSeq());
            } else {
                values.putNull("server_seq");
            }
            values.put("created_at", event.getCreatedAt());
            values.put("sync_status", event.getSyncStatus());

            long rowId = db.insert("clipboard_history", null, values);
            if (rowId < 0) {
                return InsertOutcome.FAILED;
            }
            db.setTransactionSuccessful();
            return InsertOutcome.INSERTED;
        } catch (RuntimeException e) {
            return InsertOutcome.FAILED;
        } finally {
            db.endTransaction();
        }
    }

    /**
     * 按 created_at DESC（新 -> 旧）分页查询，offset/limit 由调用方控制。
     */
    public synchronized List<ClipboardEvent> queryHistory(int offset, int limit) {
        List<ClipboardEvent> out = new ArrayList<>();
        if (limit <= 0) {
            return out;
        }
        SQLiteDatabase db = dbHelper.getReadableDatabase();
        try (Cursor c = db.query("clipboard_history", SELECT_COLUMNS,
                null, null, null, null,
                "created_at DESC, id DESC",
                (offset < 0 ? 0 : offset) + "," + limit)) {
            while (c.moveToNext()) {
                out.add(rowToEvent(c));
            }
        }
        return out;
    }

    /**
     * 服务器 ACK：记录 serverSeq / serverTime，sync_status = SYNCED（§44）。
     *
     * @return true 表示命中 eventId 并已更新
     */
    public synchronized boolean updateServerAck(String eventId, long serverSeq,
                                                long serverTime) {
        if (eventId == null || eventId.isEmpty()) {
            return false;
        }
        SQLiteDatabase db = dbHelper.getWritableDatabase();
        ContentValues values = new ContentValues();
        values.put("server_seq", serverSeq);
        values.put("server_time", serverTime);
        values.put("sync_status", ClipboardEvent.SYNC_STATUS_SYNCED);
        int rows = db.update("clipboard_history", values,
                "event_id = ?", new String[]{eventId});
        return rows > 0;
    }

    /**
     * 标记发送失败（sync_status = FAILED，保留 server_seq IS NULL 可重试，§85）。
     */
    public synchronized boolean markSyncFailed(String eventId) {
        if (eventId == null || eventId.isEmpty()) {
            return false;
        }
        SQLiteDatabase db = dbHelper.getWritableDatabase();
        ContentValues values = new ContentValues();
        values.put("sync_status", ClipboardEvent.SYNC_STATUS_FAILED);
        int rows = db.update("clipboard_history", values,
                "event_id = ? AND server_seq IS NULL",
                new String[]{eventId});
        return rows > 0;
    }

    /**
     * 待同步事件：server_seq IS NULL 且状态 PENDING/FAILED，按 id 升序（§86）。
     */
    public synchronized List<ClipboardEvent> queryPendingSync(int limit) {
        List<ClipboardEvent> out = new ArrayList<>();
        if (limit <= 0) {
            return out;
        }
        SQLiteDatabase db = dbHelper.getReadableDatabase();
        try (Cursor c = db.query("clipboard_history", SELECT_COLUMNS,
                "server_seq IS NULL AND sync_status IN (0, 2)",
                null, null, null, "id ASC", String.valueOf(limit))) {
            while (c.moveToNext()) {
                out.add(rowToEvent(c));
            }
        }
        return out;
    }

    /**
     * 历史清理：超过 maxHistoryDays 的过期删除 + 超出 maxHistoryCount 的
     * 最旧删除（§28）；参数 &lt;= 0 表示该项条件不限制。
     *
     * @return 删除行数
     */
    public synchronized int cleanup(int maxHistoryCount, int maxHistoryDays) {
        SQLiteDatabase db = dbHelper.getWritableDatabase();
        int removed = 0;
        db.beginTransaction();
        try {
            // 条件一：超过最长保存天数的过期记录（§28）
            if (maxHistoryDays > 0) {
                long deadline = System.currentTimeMillis()
                        - (long) maxHistoryDays * MILLIS_PER_DAY;
                removed += db.delete("clipboard_history",
                        "created_at < ?", new String[]{String.valueOf(deadline)});
            }
            // 条件二：超过最大条数时删除最旧记录（§28）
            if (maxHistoryCount > 0) {
                removed += db.delete("clipboard_history",
                        "id NOT IN (SELECT id FROM clipboard_history"
                                + " ORDER BY created_at DESC, id DESC"
                                + " LIMIT ?)",
                        new String[]{String.valueOf(maxHistoryCount)});
            }
            db.setTransactionSuccessful();
        } finally {
            db.endTransaction();
        }
        return removed;
    }

    /**
     * 历史记录总数。
     */
    public synchronized long count() {
        SQLiteDatabase db = dbHelper.getReadableDatabase();
        try (Cursor c = db.rawQuery("SELECT COUNT(*) FROM clipboard_history;",
                null)) {
            return c.moveToFirst() ? c.getLong(0) : 0L;
        }
    }

    // ---- 同步游标 lastServerSeq（§48）--------------------------------------

    /**
     * 读取同步游标 lastServerSeq；未初始化时返回 0。
     */
    public synchronized long getLastServerSeq() {
        SQLiteDatabase db = dbHelper.getReadableDatabase();
        try (Cursor c = db.query("app_state", new String[]{"value"},
                "key = ?",
                new String[]{DatabaseHelper.STATE_LAST_SERVER_SEQ},
                null, null, null)) {
            return c.moveToFirst() ? c.getLong(0) : 0L;
        }
    }

    /**
     * 持久化同步游标 lastServerSeq（仅允许单调递增更新）。
     *
     * @return true 表示已写入（含新值不大于旧值时的无操作成功）
     */
    public synchronized boolean setLastServerSeq(long lastServerSeq) {
        SQLiteDatabase db = dbHelper.getWritableDatabase();
        // key 已存在时 CONFLICT_IGNORE 不会更新值，必须先 UPDATE 再 INSERT
        ContentValues updateValues = new ContentValues();
        updateValues.put("value", lastServerSeq);
        updateValues.put("updated_at", System.currentTimeMillis());
        int rows = db.update("app_state", updateValues, "key = ?",
                new String[]{DatabaseHelper.STATE_LAST_SERVER_SEQ});
        if (rows > 0) {
            return true;
        }
        ContentValues insertValues = new ContentValues();
        insertValues.put("key", DatabaseHelper.STATE_LAST_SERVER_SEQ);
        insertValues.put("value", lastServerSeq);
        insertValues.put("updated_at", System.currentTimeMillis());
        return db.insertWithOnConflict("app_state", null, insertValues,
                SQLiteDatabase.CONFLICT_IGNORE) >= 0;
    }

    // ---- 内部工具 -----------------------------------------------------------

    private static ClipboardEvent rowToEvent(Cursor c) {
        ClipboardEvent ev = new ClipboardEvent();
        ev.setEventId(c.getString(0));
        ev.setDeviceId(c.getString(1));
        ev.setContentHash(c.getString(2));
        ev.setContentType(c.getInt(3));
        ev.setContent(c.getString(4));
        ev.setClientTime(c.getLong(5));
        ev.setServerTime(c.isNull(6) ? 0L : c.getLong(6));
        ev.setServerSeq(c.isNull(7) ? 0L : c.getLong(7));
        ev.setCreatedAt(c.getLong(8));
        ev.setSyncStatus(c.getInt(9));
        return ev;
    }
}
