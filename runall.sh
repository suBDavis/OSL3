#/bin/bash
echo "Usage: ./runall.sh [options]
  -l, --length [TIME]    run clients for length time
  -c, --numclients [NUM] number of client threads
  -n, --count [NUM]      run num iterations"
make clean
make
TIME=30
COUNT=10
THREADS=1
while [[ $# -gt 1 ]]
do
    key="$1"
    case $key in
        -l|--length)
            TIME="$2"
            shift
            ;;
        -c|--numclients)
            THREADS="$2"
            shift
            ;;
        -n|--count)
            COUNT=$2
            ;;
        *)
            ;;
    esac
    shift
done
while [ $COUNT -gt 0 ]
do
    echo "running dns-mutex"
    ./dns-mutex -c $THREADS -t -l $TIME
    echo "dns-mutex complete"
    echo "running dns-rw"
    ./dns-rw -c $THREADS -t -l $TIME
    echo "dns-rw complete"
    echo "running dns-fine"
    ./dns-fine -c $THREADS -t -l $TIME
    echo "dns-fine complete"
    let COUNT-=1
done
echo "done"

