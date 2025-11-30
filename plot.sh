python plot_metrics.py \
    --files stat.log.version0,stat.log.version1,stat.log.version2,stat.log.version3-2,stat.log.version3-3,stat.log.version3,stat.log.version4,stat.log.version5 \
    --labels FIFO,WarmAge,Greedy,CB_Hot,CB_Cold,CB_Warm,Hot_Cold_Active,Age_Active \
    --y 'evicted_blocks * 4096' \
    --ylabel 'Evicted Bytes (GB)' \
    --x 'write_size_to_cache' \
    --xlabel "Write Bytes (GB)" \
    --xscale GB \
    --yscale GB \
    --title 'Evicted Blocks Comparison'

    python plot_metrics.py \
    --files stat.log.version0,stat.log.version1,stat.log.version2,stat.log.version3-2,stat.log.version3-3,stat.log.version3,stat.log.version4,stat.log.version5 \
    --labels FIFO,WarmAge,Greedy,CB_Hot,CB_Cold,CB_Warm,Hot_Cold_Active,Age_Active \
    --y 'compacted_blocks * 4096' \
    --ylabel 'Compacted Bytes (GB)' \
    --x 'write_size_to_cache' \
    --xlabel "Write Bytes (GB)" \
    --xscale GB \
    --yscale GB \
    --title 'Compacted Blocks Comparison' \
    --output output2.png

    python histogram.py \
    --files stat.log.version0,stat.log.version1,stat.log.version2,stat.log.version3-2,stat.log.version3-3,stat.log.version3,stat.log.version4,stat.log.version5 \
    --labels FIFO,WarmAge,Greedy,CB_Hot,CB_Cold,CB_Warm,Hot_Cold_Active,Age_Active \
    --histogram_metric compacted_ages_with_segment \
    --rank 20 \
    --ylabel "count" \
    --output compacted_ages_summary.png

    python histogram.py \
    --files stat.log.version0,stat.log.version1,stat.log.version2,stat.log.version3-2,stat.log.version3-3,stat.log.version3,stat.log.version4,stat.log.version5 \
    --labels FIFO,WarmAge,Greedy,CB_Hot,CB_Cold,CB_Warm,Hot_Cold_Active,Age_Active \
    --histogram_metric evicted_ages \
    --rank 20 \
    --ylabel "count" \
    --output evicted_ages_summary.png

    python plot_metrics.py \
    --files stat.log.version0,stat.log.version1,stat.log.version2,stat.log.version3-2,stat.log.version3-3,stat.log.version3,stat.log.version4,stat.log.version5 \
    --labels FIFO,WarmAge,Greedy,CB_Hot,CB_Cold,CB_Warm,Hot_Cold_Active,Age_Active \
    --y 'global_valid_blocks/total_cache_size*4096*4096 * 100' \
    --ylabel '%' \
    --x 'write_size_to_cache' \
    --xlabel "Write Bytes (GB)" \
    --xscale GB \
    --title 'Valid Block Rate' \
    --output "valid_block.png"


 python plot_metrics.py \
    --files stat.log.version0,stat.log.version1,stat.log.version2,stat.log.version3-2,stat.log.version3-3,stat.log.version3,stat.log.version4,stat.log.version5 \
    --labels FIFO,WarmAge,Greedy,CB_Hot,CB_Cold,CB_Warm,Hot_Cold_Active,Age_Active \
    --y 'reinsert_blocks * 4096' \
    --ylabel 'Reinserted Bytes (GB)' \
    --x 'write_size_to_cache' \
    --xlabel "Write Bytes (GB)" \
    --xscale GB \
    --yscale GB \
    --title 'Reinserted bytes' \
    --output "reinserted_bytes.png"


    python plot_metrics.py \
    --files stat.log.20250818_170203,stat.log.20250818_165826 \
    --labels FIFO,Age_Active \
    --y 'evicted_blocks * 4096' \
    --ylabel 'Evicted Bytes (GB)' \
    --x 'write_size_to_cache' \
    --xlabel "Write Bytes (GB)" \
    --xscale GB \
    --yscale GB \
    --title 'Evicted Blocks Comparison' \
    --output "evicted_blocks_2.png"


    python plot_metrics.py \
    --files stat.log.20250818_170203,stat.log.20250818_165826 \
    --labels FIFO,Age_Active \
    --y 'compacted_blocks * 4096' \
    --ylabel 'Compacted Bytes (GB)' \
    --x 'write_size_to_cache' \
    --xlabel "Write Bytes (GB)" \
    --xscale GB \
    --yscale GB \
    --title 'Compacted Blocks Comparison' \
    --output "compacted_blocks_2.png"

  python plot_metrics.py \
    --files stat.log.20250818_214512,stat.log.20250818_231203,stat.log.20250818_210101,stat.log.20250818_223352,stat.log.20250825_235728 \
    --labels FIFO,Greedy_80%,Age_Active_93%,Age_Active_80%,Age_Active_Dynamic \
    --y 'evicted_blocks * 4096' \
    --ylabel 'Evicted Bytes (GB)' \
    --x 'write_size_to_cache' \
    --xlabel "Write Bytes (GB)" \
    --xscale GB \
    --yscale GB \
    --title 'Evicted Blocks Comparison' \
    --output "evicted_blocks_3.png"

   python plot_metrics.py \
    --files stat.log.20250818_214512,stat.log.20250818_231203,stat.log.20250818_210101,stat.log.20250818_223352,stat.log.20250825_235728 \
    --labels FIFO,Greedy_80%,Age_Active_93%,Age_Active_80%,Age_Active_Dynamic \
    --y 'compacted_blocks * 4096' \
    --ylabel 'Compacted Bytes (GB)' \
    --x 'write_size_to_cache' \
    --xlabel "Write Bytes (GB)" \
    --xscale GB \
    --yscale GB \
    --title 'Compacted Blocks Comparison' \
    --output "compacted_blocks_3.png"

   python plot_metrics.py \
    --files stat.log.20250818_214512,stat.log.20250818_231203,stat.log.20250818_210101,stat.log.20250818_223352,stat.log.20250825_235728 \
    --labels FIFO,Greedy_80%,Age_Active_93%,Age_Active_80%,Age_Active_Dynamic \
    --y 'global_valid_blocks/total_cache_size*4096*4096 * 100' \
    --ylabel '%' \
    --x 'write_size_to_cache' \
    --xlabel "Write Bytes (GB)" \
    --xscale GB \
    --title 'Valid Block Rate' \
    --output "valid_block_rate_3.png"

    python histogram.py \
    --files stat.log.20250818_214512,stat.log.20250818_231203,stat.log.20250818_210101,stat.log.20250818_223352 \
    --labels FIFO,Greedy_80%,Age_Active_93%,Age_Active_80% \
    --histogram_metric compacted_ages_with_segment \
    --rank 20 \
    --ylabel "count" \
    --output compacted_ages_summary_3.png


    python histogram.py \
    --files stat.log.20250818_214512,stat.log.20250818_231203,stat.log.20250818_210101,stat.log.20250818_223352 \
    --labels FIFO,Greedy_80%,Age_Active_93%,Age_Active_80% \
    --histogram_metric evicted_ages \
    --rank 40 \
    --ylabel "count" \
    --output evicted_ages_summary_3.png


 python plot_metrics.py \
    --files stat.log.20250818_214512,stat.log.20250818_231203,stat.log.20250818_210101,stat.log.20250818_223352 \
    --labels FIFO,Greedy_80%,Age_Active_93%,Age_Active_80% \
    --y 'reinsert_blocks * 4096' \
    --ylabel 'Reinserted Bytes (GB)' \
    --x 'write_size_to_cache' \
    --xlabel "Write Bytes (GB)" \
    --xscale GB \
    --yscale GB \
    --title 'Reinserted bytes' \
    --output "reinserted_3_bytes.png"