# spinel: wasm-abort-raises: Reentry
class Reentry < StandardError
  def message
    puts "report once"
    raise Reentry
  end
end

raise Reentry
