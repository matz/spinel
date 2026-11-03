class RescueScopedDb
  def busy_timeout=(ms)
    "native #{ms}"
  end
end
