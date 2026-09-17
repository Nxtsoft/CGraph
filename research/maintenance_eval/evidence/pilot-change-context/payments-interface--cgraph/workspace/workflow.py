from pricing import amount_due


def quote_invoice(subtotal_cents, shipping_cents, discount_percent):
    invoice = {
        "subtotal_cents": subtotal_cents,
        "shipping_cents": shipping_cents,
        "discount_percent": discount_percent,
    }
    return {"total_cents": amount_due(invoice)}
