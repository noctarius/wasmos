#ifndef WASMOS_LIBUI_ROW_H
#define WASMOS_LIBUI_ROW_H

/* libui_row.h - Generic horizontal row container layout. */

static inline void
ui_layout_row(ui_context_t *ctx, ui_component_t *p)
{
    int32_t x = p->bounds.x + p->padding_px;
    const int32_t y = p->bounds.y + p->padding_px;
    const int32_t inner_w = p->bounds.w - (p->padding_px * 2);
    const int32_t inner_h = p->bounds.h - (p->padding_px * 2);
    int32_t child_count = 0;
    int32_t fixed_w = 0;
    int32_t child_id = p->first_child_id;

    while (child_id > 0) {
        ui_component_t *c = ui_component_by_id(ctx, child_id);
        if (!c) break;
        child_count++;
        if (c->next_sibling_id > 0) {
            const int32_t w = c->preferred_h > 8 ? c->preferred_h : 8;
            fixed_w += w;
        }
        child_id = c->next_sibling_id;
    }

    if (child_count > 1) fixed_w += p->gap_px * (child_count - 1);
    int32_t remaining_w = inner_w - fixed_w;
    if (remaining_w < 8) remaining_w = 8;

    child_id = p->first_child_id;
    while (child_id > 0) {
        ui_component_t *c = ui_component_by_id(ctx, child_id);
        if (!c) break;
        const int32_t w = (c->next_sibling_id > 0)
            ? (c->preferred_h > 8 ? c->preferred_h : 8)
            : remaining_w;
        c->bounds.x = x;
        c->bounds.y = y;
        c->bounds.w = w;
        c->bounds.h = inner_h > 8 ? inner_h : 8;
        {
            const ui_component_ops_t *child_ops = &ui_component_ops[c->type];
            if (child_ops->layout) {
                child_ops->layout(ctx, c);
            } else if (c->first_child_id > 0) {
                ui_layout_vertical(ctx, c->id);
            }
        }
        x += w + p->gap_px;
        child_id = c->next_sibling_id;
    }
}

#endif /* WASMOS_LIBUI_ROW_H */
