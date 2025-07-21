# Twixuss' IMage format

Compress images by:
1. Computing mip map and deltas between it and original image.
3. Comressing deltas, which are usually small.
4. Compressing the mipmap again until 1x1.

Comparison with other formats:
<details>
<summary>Setting</summary>

* 4k image
* no IO
* 1 warmup
* average of 8 runs
* AVX2
</details>

Format|Size|Percentage|Graph
-|-|-|-
jpg|10.1 MiB|21%|█████
qoi|20 MiB|41%|█████████
png|22.1 MiB|45%|██████████
tim|23 MiB|48%|██████████
tga|40.3 MiB|83%|█████████████████
bmp|48 MiB|100%|████████████████████

Format|Dec. speed|Percentage|Graph
-|-|-|-
bmp|596 MiB/s|100%|█████████████████████
qoi|422 MiB/s|70%|███████████████
tim|292 MiB/s|49%|██████████
tga|288 MiB/s|48%|██████████
jpg|202 MiB/s|33%|███████
png|126 MiB/s|21%|█████

Format|Enc. speed|Percentage|Graph
-|-|-|-
bmp|627 MiB/s|100%|█████████████████████
tga|451 MiB/s|71%|███████████████
qoi|178 MiB/s|28%|██████
tim|143 MiB/s|22%|█████
jpg|117 MiB/s|18%|████
png|18 MiB/s|2%|█