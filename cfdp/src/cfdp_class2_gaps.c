/*
 * cfdp_class2_gaps.c — growable sorted list of missing byte ranges
 *
 * Shared by the Class 2 sender and receiver.  See cfdp_class2.h for the
 * contract.  All ranges are half-open [start, end).
 */

#include "cfdp_class2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Ensure room for at least `needed` entries. */
static cfdp_status_t gap_list_reserve(cfdp2_gap_list_t *gl, uint32_t needed)
{
    if (needed <= gl->capacity) return CFDP_OK;

    uint32_t new_cap = gl->capacity ? gl->capacity : CFDP_GAP_LIST_INITIAL;
    while (new_cap < needed) new_cap *= 2u;

    cfdp_nak_gap_t *p = realloc(gl->gaps, (size_t)new_cap * sizeof(*p));
    if (!p) {
        fprintf(stderr, "[GAPS] OOM growing gap list to %u entries\n", new_cap);
        return CFDP_ERR_IO;
    }
    gl->gaps     = p;
    gl->capacity = new_cap;
    return CFDP_OK;
}

cfdp_status_t cfdp2_gap_list_init(cfdp2_gap_list_t *gl)
{
    gl->gaps     = NULL;
    gl->count    = 0;
    gl->capacity = 0;
    return gap_list_reserve(gl, CFDP_GAP_LIST_INITIAL);
}

void cfdp2_gap_list_free(cfdp2_gap_list_t *gl)
{
    free(gl->gaps);
    gl->gaps     = NULL;
    gl->count    = 0;
    gl->capacity = 0;
}

cfdp_status_t cfdp2_gap_list_reset(cfdp2_gap_list_t *gl, uint32_t file_size)
{
    gl->count = 0;
    if (file_size == 0) return CFDP_OK;
    return cfdp2_gap_list_add(gl, 0, file_size);
}

/*
 * Remove [recv_start, recv_end) from every overlapping gap.
 *
 *   covers whole gap      -> delete it
 *   covers left part      -> shrink from left
 *   covers right part     -> shrink from right
 *   strictly inside       -> split into two (needs one extra slot)
 *
 * The list is sorted and non-overlapping, so at most one gap can be
 * split; we reserve the extra slot up front so a failed grow leaves the
 * list untouched.
 */
cfdp_status_t cfdp2_gap_list_mark_received(cfdp2_gap_list_t *gl,
                                           uint32_t recv_start,
                                           uint32_t recv_end)
{
    if (recv_end <= recv_start) return CFDP_OK;

    cfdp_status_t rc = gap_list_reserve(gl, gl->count + 1u);
    if (rc != CFDP_OK) return rc;

    for (uint32_t i = 0; i < gl->count; ) {
        uint32_t gs = gl->gaps[i].start;
        uint32_t ge = gl->gaps[i].end;

        if (recv_end <= gs) break;            /* sorted: nothing further overlaps */
        if (recv_start >= ge) { i++; continue; }

        bool trims_left  = recv_start <= gs;
        bool trims_right = recv_end   >= ge;

        if (trims_left && trims_right) {
            memmove(&gl->gaps[i], &gl->gaps[i + 1],
                    (gl->count - i - 1) * sizeof(cfdp_nak_gap_t));
            gl->count--;
        } else if (trims_left) {
            gl->gaps[i].start = recv_end;
            i++;
        } else if (trims_right) {
            gl->gaps[i].end = recv_start;
            i++;
        } else {
            memmove(&gl->gaps[i + 2], &gl->gaps[i + 1],
                    (gl->count - i - 1) * sizeof(cfdp_nak_gap_t));
            gl->count++;
            gl->gaps[i    ].end   = recv_start;
            gl->gaps[i + 1].start = recv_end;
            gl->gaps[i + 1].end   = ge;
            i += 2;
        }
    }
    return CFDP_OK;
}

/*
 * Insert [start, end), coalescing with any gap it overlaps or touches.
 * Used by the sender to merge NAKs; also by reset().
 */
cfdp_status_t cfdp2_gap_list_add(cfdp2_gap_list_t *gl,
                                 uint32_t start, uint32_t end)
{
    if (end <= start) return CFDP_OK;

    cfdp_status_t rc = gap_list_reserve(gl, gl->count + 1u);
    if (rc != CFDP_OK) return rc;

    /* Find first gap whose end >= start (candidate for merge / insertion) */
    uint32_t i = 0;
    while (i < gl->count && gl->gaps[i].end < start) i++;

    /* Absorb every gap that overlaps or touches [start, end) */
    uint32_t j = i;
    while (j < gl->count && gl->gaps[j].start <= end) {
        if (gl->gaps[j].start < start) start = gl->gaps[j].start;
        if (gl->gaps[j].end   > end)   end   = gl->gaps[j].end;
        j++;
    }

    /* Replace gaps [i, j) with the single merged gap */
    uint32_t removed = j - i;
    if (removed == 0) {
        memmove(&gl->gaps[i + 1], &gl->gaps[i],
                (gl->count - i) * sizeof(cfdp_nak_gap_t));
        gl->count++;
    } else if (removed > 1) {
        memmove(&gl->gaps[i + 1], &gl->gaps[j],
                (gl->count - j) * sizeof(cfdp_nak_gap_t));
        gl->count -= (removed - 1);
    }
    gl->gaps[i].start = start;
    gl->gaps[i].end   = end;
    return CFDP_OK;
}
