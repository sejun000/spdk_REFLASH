python3 ./plot_metrics.py --files /mnt/nvme2n2/stat.log.20250728_133441,/mnt/nvme2n2/stat.log.20250729_140231 --labels FIFO,Greedy --y delta_invalidate_blocks*4096/delta_write_size_to_cache --title Trace_Comparison --xscale GB --ylabel invalidate_ratio


python3 ./plot_metrics.py --files /mnt/nvme2n2/stat.log.20250728_133441,/mnt/nvme2n2/stat.log.20250729_140231 --labels FIFO,Greedy --y global_valid_blocks*4096*4096/total_cache_size --title Trac --xscale GB --ylabel valid_page_ratio

