def amount_due(subtotal_cents, shipping_cents, discount_percent):
    """Return the invoice total in integer cents."""
    return (subtotal_cents + shipping_cents) * (100 - discount_percent) // 100


def maintenance_update_probe():
    return 731
