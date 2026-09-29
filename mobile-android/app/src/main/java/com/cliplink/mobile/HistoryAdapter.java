package com.cliplink.mobile;

import android.graphics.Color;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.TextView;

import androidx.annotation.NonNull;
import androidx.recyclerview.widget.RecyclerView;

import com.cliplink.mobile.protocol.ClipboardEvent;

import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Calendar;
import java.util.List;
import java.util.Locale;

/**
 * 剪贴板历史列表适配器（§62 UI 与业务分离：仅负责展示与点击回调）。
 *
 * - 内容截断预览：最多 2 行，超长不撑爆 UI（§8 / §75）
 * - 本地时间：UTC 毫秒 -> 本地 HH:mm，非今天显示 MM-dd HH:mm（§80）
 * - 同步状态标记：待同步 / 已同步 / 同步失败（§85）
 * - 点击条目回调交给 Activity 走 HISTORY 路径（§76 / §83）
 */
public class HistoryAdapter extends RecyclerView.Adapter<HistoryAdapter.Holder> {

    /** 条目点击回调（携带完整内容文本） */
    public interface OnItemClickListener {
        void onItemClick(ClipboardEvent event);
    }

    private final List<ClipboardEvent> items = new ArrayList<>();
    private final OnItemClickListener listener;

    private final SimpleDateFormat timeToday =
            new SimpleDateFormat("HH:mm", Locale.getDefault());
    private final SimpleDateFormat timeOther =
            new SimpleDateFormat("MM-dd HH:mm", Locale.getDefault());

    public HistoryAdapter(OnItemClickListener listener) {
        this.listener = listener;
    }

    /** 整表替换（查询结果按 created_at DESC，§75） */
    public void submitList(List<ClipboardEvent> data) {
        items.clear();
        if (data != null) {
            items.addAll(data);
        }
        notifyDataSetChanged();
    }

    @Override
    public int getItemCount() {
        return items.size();
    }

    @NonNull
    @Override
    public Holder onCreateViewHolder(@NonNull ViewGroup parent, int viewType) {
        View view = LayoutInflater.from(parent.getContext())
                .inflate(R.layout.item_history, parent, false);
        return new Holder(view);
    }

    @Override
    public void onBindViewHolder(@NonNull Holder holder, int position) {
        final ClipboardEvent event = items.get(position);

        // 内容预览：换行折叠，最多 2 行截断（§8）
        String content = event.getContent().replace('\n', ' ')
                .replace('\r', ' ');
        holder.tvContent.setText(content);
        holder.tvTime.setText(formatTime(event.getCreatedAt()));
        bindSyncStatus(holder.tvSyncStatus, event.getSyncStatus());

        holder.itemView.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                if (listener != null) {
                    listener.onItemClick(event);
                }
            }
        });
    }

    /** 本地时间显示（§80：UTC 毫秒转本地；今天只显示时分） */
    private String formatTime(long utcMillis) {
        if (utcMillis <= 0) {
            return "";
        }
        Calendar now = Calendar.getInstance();
        Calendar item = Calendar.getInstance();
        item.setTimeInMillis(utcMillis);
        boolean sameDay = now.get(Calendar.YEAR) == item.get(Calendar.YEAR)
                && now.get(Calendar.DAY_OF_YEAR) == item.get(Calendar.DAY_OF_YEAR);
        return (sameDay ? timeToday : timeOther).format(item.getTime());
    }

    /** 同步状态标记（§85） */
    private static void bindSyncStatus(TextView tv, int syncStatus) {
        switch (syncStatus) {
            case ClipboardEvent.SYNC_STATUS_SYNCED:
                tv.setText(R.string.sync_status_synced);
                tv.setTextColor(Color.parseColor("#2E7D32"));
                break;
            case ClipboardEvent.SYNC_STATUS_FAILED:
                tv.setText(R.string.sync_status_failed);
                tv.setTextColor(Color.parseColor("#C62828"));
                break;
            case ClipboardEvent.SYNC_STATUS_PENDING:
            default:
                tv.setText(R.string.sync_status_pending);
                tv.setTextColor(Color.parseColor("#F9A825"));
                break;
        }
    }

    static class Holder extends RecyclerView.ViewHolder {
        final TextView tvContent;
        final TextView tvTime;
        final TextView tvSyncStatus;

        Holder(@NonNull View itemView) {
            super(itemView);
            tvContent = itemView.findViewById(R.id.tv_content);
            tvTime = itemView.findViewById(R.id.tv_time);
            tvSyncStatus = itemView.findViewById(R.id.tv_sync_status);
        }
    }
}
