def amount_due(subtotal_cents, shipping_cents, discount_percent):
    """Return the invoice total in integer cents."""
    discount_cents = subtotal_cents * discount_percent // 100
    return subtotal_cents - discount_cents + shipping_cents
