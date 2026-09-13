from pricing import amount_due


def render_receipt(subtotal_cents, shipping_cents, discount_percent):
    total = amount_due(subtotal_cents, shipping_cents, discount_percent)
    return f"Total: {total} cents"
