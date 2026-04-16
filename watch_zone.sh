watch -n 1 'sudo nvme zns report-zone /dev/nvme3n2 | awk '\''{
    for(i=1;i<=NF;i++){
        if($i=="State:"){
            count[$(i+1)]++

            if($(i+1)=="0x20"){
                slba = strtonum($2)
                wp   = strtonum($4)
                cap  = strtonum($6)

                zone = slba / strtonum("0x400000")
                usage = (wp - slba) / cap * 100

                line = sprintf("Zone: %-5d SLBA: %-12s WP: %-12s Usage: %6.2f%%",
                               zone, $2, $4, usage)

                if(zone==0 || zone==1){
                    r[++r_idx] = line
                }
                else if(zone>=2 && zone<=17){
                    i_layer[++i_idx] = line
                }
                else if(zone>=18 && zone<2902){
                    l_hot[++h_idx] = line
                }
                else if(zone>=2902){
                    l_cold[++c_idx] = line
                }
            }
        }
    }
}
END{
    for(s in count) print s, count[s]

    print "---- 0x20 (opened) zones ----"

    print "# RLayer"
    for(i=1;i<=r_idx;i++) print r[i]

    print "\n# ILayer"
    for(i=1;i<=i_idx;i++) print i_layer[i]

    print "\n# LLayer - Hot"
    for(i=1;i<=h_idx;i++) print l_hot[i]

    print "\n# LLayer - Cold"
    for(i=1;i<=c_idx;i++) print l_cold[i]
}'\'''