module RescueRunsPragmas
  def busy_timeout = "pragma get"
  def busy_timeout=(ms)
    "pragma set #{ms}"
  end
end
