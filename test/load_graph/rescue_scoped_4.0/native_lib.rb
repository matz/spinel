class RescueScopedDb
  def busy_timeout=(ms)
    "native40 #{ms}"
  end
end
