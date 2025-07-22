# Twixuss' IMage format

Steps:
1. If the source image is not power of two square, pad it with black.
2. For each 2x2 quad in source image:
   1. Compute min and max and store their average in corresponding location in another image.
   2. Compute deltas between those 4 pixels and the average and write them into a bitstream with minimum number of bits.
3. Now you have an image of averages that is 4 times smaller than the original and a bitstream of deltas. Repeat the previous step with averages as input, until total compressed size becomes larger than of previous level or until 1x1.

Comparison with other formats:
<details>
<summary>Setting</summary>

* 4k image
* no IO
* 1 warmup
* average of 8 runs
* AVX2
* 12 threads
</details>

Format|Size|Percentage|Graph
-|-|-|-
jpg|10.1 MiB|21%|█████
qoi|20 MiB|41%|█████████
png|22.1 MiB|45%|██████████
tim|23.9 MiB|49%|██████████
tga|40.3 MiB|83%|█████████████████
bmp|48 MiB|100%|████████████████████

Format|Dec. speed|Percentage|Graph
-|-|-|-
bmp|619 MiB/s|100%|█████████████████████
qoi|434 MiB/s|70%|███████████████
tim|390 MiB/s|63%|█████████████
tga|290 MiB/s|46%|██████████
jpg|201 MiB/s|32%|███████
png|128 MiB/s|20%|█████

Format|Enc. speed|Percentage|Graph
-|-|-|-
bmp|597 MiB/s|100%|█████████████████████
tga|455 MiB/s|76%|████████████████
qoi|181 MiB/s|30%|███████
tim|133 MiB/s|22%|█████
jpg|117 MiB/s|19%|████
png|18 MiB/s|3%|█