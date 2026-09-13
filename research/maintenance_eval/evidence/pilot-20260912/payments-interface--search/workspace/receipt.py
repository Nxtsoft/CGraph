from pricing import amount_due


def render_receipt(subtotal_cents, shipping_cents, discount_percent):
    invoice = {
        "subtotal_cents": subtotal_cents,
        "shipping_cents": shipping_cents,
        "discount_percent": discount_percent,
    }
    total = amount_due(invoice)
    return f"Total: {total} cents"
