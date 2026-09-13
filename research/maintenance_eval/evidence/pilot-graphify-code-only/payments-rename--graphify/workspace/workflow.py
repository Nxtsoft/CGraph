from pricing import net_due


def quote_invoice(subtotal_cents, shipping_cents, discount_percent):
    return {"total_cents": net_due(subtotal_cents, shipping_cents, discount_percent)}
