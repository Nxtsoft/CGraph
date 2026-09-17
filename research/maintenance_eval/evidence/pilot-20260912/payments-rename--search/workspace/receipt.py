from pricing import net_due


def render_receipt(subtotal_cents, shipping_cents, discount_percent):
    total = net_due(subtotal_cents, shipping_cents, discount_percent)
    return f"Total: {total} cents"
