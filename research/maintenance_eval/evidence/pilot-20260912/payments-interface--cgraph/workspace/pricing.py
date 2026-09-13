def amount_due(invoice):
    """Return the invoice total in integer cents."""
    return (
        invoice["subtotal_cents"] + invoice["shipping_cents"]
    ) * (100 - invoice["discount_percent"]) // 100
