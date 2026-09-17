from workflow import quote_invoice


def checkout(subtotal_cents, shipping_cents, discount_percent):
    return quote_invoice(subtotal_cents, shipping_cents, discount_percent)
