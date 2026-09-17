from pricing import amount_due


def quote_invoice(subtotal_cents, shipping_cents, discount_percent):
    return {"total_cents": amount_due(subtotal_cents, shipping_cents, discount_percent)}
