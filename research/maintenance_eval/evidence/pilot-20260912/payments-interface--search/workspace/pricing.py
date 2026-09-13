def amount_due(invoice):
    """Return the invoice total in integer cents."""
    subtotal_cents = invoice["subtotal_cents"]
    shipping_cents = invoice["shipping_cents"]
    discount_percent = invoice["discount_percent"]
    return (subtotal_cents + shipping_cents) * (100 - discount_percent) // 100
