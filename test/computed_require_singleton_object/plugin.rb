UNDEFINED = Object.new
def UNDEFINED.inspect = "UNDEFINED"

module Opts
  def self.make
    opts = Object.new
    def opts.topdict = :top
    opts
  end
end
