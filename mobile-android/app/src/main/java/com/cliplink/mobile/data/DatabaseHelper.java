package com.cliplink.mobile.data;

import android.content.Context;
import android.database.sqlite.SQLiteDatabase;
import android.database.sqlite.SQLiteOpenHelper;

/**
 * SQLite 数据库助手（见需求 §25-§26 / §48 / §59）。
 *
 * - clipboard_history：结构与 PC 端 §25 字段完全一致 + §26 三条索引，
 *   并含 §85 sync_status 列（与 PC 端 DatabaseManager 建表一致）
 * - app_state：持久化同步游标 lastServerSeq 等键值状态（与 PC 端一致：
 *   key TEXT PRIMARY KEY, value INTEGER NOT NULL, updated_at INTEGER NOT NULL）
 *
 * 建表策略（修复 no such table: app_state 崩溃）：
 * - 全部建表语句为单条 + IF NOT EXISTS 幂等写法，逐条 execSQL 执行
 *   （Android execSQL 一次只执行单条语句，多语句拼接会导致后续表缺失）
 * - ensureSchema(db) 可由任意路径显式调用；onCreate / onUpgrade / onOpen
 *   三处均调用，保证无论走哪条生命周期、无论库新旧，表与索引一定存在。
 */
public class DatabaseHelper extends SQLiteOpenHelper {

    public static final String DB_NAME = "cliplink.db";
    public static final int DB_VERSION = 1;

    /** app_state 键：已成功同步到的服务器序号（见需求 §48） */
    public static final String STATE_LAST_SERVER_SEQ = "lastServerSeq";

    /** 单条幂等建表/建索引语句（禁止拼接成一条多语句字符串） */
    private static final String[] CREATE_STATEMENTS = {
            "CREATE TABLE IF NOT EXISTS clipboard_history ("
                    + "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
                    + "  event_id TEXT NOT NULL UNIQUE,"
                    + "  device_id TEXT NOT NULL,"
                    + "  content_hash TEXT NOT NULL,"
                    + "  content_type INTEGER NOT NULL,"
                    + "  content TEXT NOT NULL,"
                    + "  client_time INTEGER,"
                    + "  server_time INTEGER,"
                    + "  server_seq INTEGER,"
                    + "  created_at INTEGER NOT NULL,"
                    + "  sync_status INTEGER NOT NULL DEFAULT 0"
                    + ")",
            "CREATE INDEX IF NOT EXISTS idx_clipboard_created "
                    + "ON clipboard_history(created_at)",
            "CREATE INDEX IF NOT EXISTS idx_clipboard_hash "
                    + "ON clipboard_history(content_hash)",
            "CREATE INDEX IF NOT EXISTS idx_clipboard_server_seq "
                    + "ON clipboard_history(server_seq)",
            "CREATE TABLE IF NOT EXISTS app_state ("
                    + "  key TEXT PRIMARY KEY,"
                    + "  value INTEGER NOT NULL,"
                    + "  updated_at INTEGER NOT NULL"
                    + ")",
    };

    public DatabaseHelper(Context context) {
        super(context, DB_NAME, null, DB_VERSION);
    }

    /**
     * 幂等补建全部表与索引（IF NOT EXISTS，重复执行无副作用）。
     * 供 onCreate / onUpgrade / onOpen 以及任何需要兜底的路径调用。
     */
    public static void ensureSchema(SQLiteDatabase db) {
        if (db == null) {
            return;
        }
        for (String sql : CREATE_STATEMENTS) {
            db.execSQL(sql);
        }
    }

    @Override
    public void onCreate(SQLiteDatabase db) {
        ensureSchema(db);
    }

    @Override
    public void onUpgrade(SQLiteDatabase db, int oldVersion, int newVersion) {
        // MVP 仅 v1；后续版本在此按版本序迁移，禁止直接 DROP（§72 不丢数据）。
        // 同时补齐旧库中缺失的表/索引（幂等，不触碰已有数据）。
        ensureSchema(db);
    }

    @Override
    public void onOpen(SQLiteDatabase db) {
        super.onOpen(db);
        // 兜底：历史版本只建了部分表的旧库不会再走 onCreate/onUpgrade，
        // 每次打开库时幂等补建缺失表，保证 app_state / clipboard_history 一定存在。
        if (!db.isReadOnly()) {
            ensureSchema(db);
        }
    }
}
