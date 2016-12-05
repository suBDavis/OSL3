#/bin/bash
make clean
make
TIME=30
COUNT=10
while [[ $# -gt 1 ]]
do
    key="$1"
    case $key in
        -t|--time)
            TIME="$2"
            shift
            ;;
        -c|--count)
            COUNT=$2
            ;;
        *)
            ;;
    esac
    shift
done
while [ $COUNT -gt 0 ]; do
    echo "running dns-mutex"
    ./dns-mutex -c 15 -t -l $TIME
    echo "dns-mutex complete"
    echo "running dns-rw"
    ./dns-rw -c 15 -t -l $TIME
    echo "dns-rw complete"
    echo "running dns-fine"
    ./dns-fine -c 15 -t -l $TIME
    echo "dns-fine complete"
    let COUNT-=1
done
