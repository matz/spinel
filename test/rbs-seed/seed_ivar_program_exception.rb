# An --rbs pin naming the program's own exception class: its instances are
# exceptions, what a rescue binds, so the pin laid out as an object of the
# class refused the rescued value (no conversion keeps an Exception in it).
class SeedPinOwnExcErr < StandardError
end

class SeedPinOwnExcHolder
  attr_accessor :err

  def run(fail_now)
    begin
      raise SeedPinOwnExcErr, "boom" if fail_now
    rescue SeedPinOwnExcErr => e
      @err = e
    end
    self
  end
end

h = SeedPinOwnExcHolder.new.run(ARGV.size == 0)
p h.err.class
p h.err.message
