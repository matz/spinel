# A fresh String receiver cannot share its append through tap.
p((+"a").tap { |x| x << "!" })
