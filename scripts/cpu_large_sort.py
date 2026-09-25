"""Deterministic model of the saved large ordering operation.

Preserves the pivot/equal-key movement order, rather than substituting a stable
sort. Binary discovery remains upstream. This is an offline computational model,
not a live-memory permission or a certified x64-to-GPU compiler.
"""
from cpu_sort_contract import key, validate_projection, MAX_BYTES

def exact_sort(data, bits, ideal=None):
    validate_projection(bits)
    if len(data)%16 or len(data)>MAX_BYTES:raise ValueError('Record budget/stride')
    records=[data[i:i+16] for i in range(0,len(data),16)]
    values=[(key(r,bits),r) for r in records]
    ideal=len(values) if ideal is None else ideal
    if not isinstance(ideal,int) or not 0<=ideal<=len(values):raise ValueError('Ideal bound')
    stats={'partitions':0,'median3':0,'insertion_ranges':0,'heap_ranges':0,'swaps':0}
    def swap(x,y):
        values[x],values[y]=values[y],values[x];stats['swaps']+=1
    def med(a,b,c):
        stats['median3']+=1
        if values[b][0]<values[a][0]:swap(a,b)
        if values[c][0]<values[b][0]:
            swap(b,c)
            if values[b][0]<values[a][0]:swap(a,b)
    def partition(lo,hi):
        stats['partitions']+=1
        mid=lo+(hi-lo)//2;last=hi-1
        if last-lo>40:
            step=(hi-lo)//8
            med(lo,lo+step,lo+2*step);med(mid-step,mid,mid+step)
            med(last-2*step,last-step,last);med(lo+step,mid,last-step)
        else:med(lo,mid,last)
        left=mid;right=mid+1
        while lo<left and values[left-1][0]==values[left][0]:left-=1
        while right<hi and values[right][0]==values[left][0]:right+=1
        scan_up=right;scan_down=left
        while True:
            while scan_up<hi:
                if values[left][0]<values[scan_up][0]:pass
                elif values[scan_up][0]<values[left][0]:break
                else:
                    if right!=scan_up:swap(right,scan_up)
                    right+=1
                scan_up+=1
            while lo<scan_down:
                prev=scan_down-1
                if values[prev][0]<values[left][0]:pass
                elif values[left][0]<values[prev][0]:break
                else:
                    left-=1
                    if left!=prev:swap(left,prev)
                scan_down-=1
            if scan_down==lo and scan_up==hi:return left,right
            if scan_down==lo:
                if right!=scan_up:swap(left,right)
                right+=1;swap(left,scan_up);left+=1;scan_up+=1
            elif scan_up==hi:
                scan_down-=1;left-=1
                if scan_down!=left:swap(scan_down,left)
                right-=1;swap(left,right)
            else:
                scan_down-=1;swap(scan_up,scan_down);scan_up+=1
    def insertion(lo,hi):
        stats['insertion_ranges']+=1
        for i in range(lo+1,hi):
            v=values[i];hole=i
            while hole>lo and v[0]<values[hole-1][0]:
                values[hole]=values[hole-1];hole-=1
            values[hole]=v
    def heap(lo,hi):
        stats['heap_ranges']+=1
        def hole_down(hole,n,v):
            top=hole
            while hole<(n-1)//2:
                child=2*hole+2
                if values[lo+child][0]<values[lo+child-1][0]:child-=1
                values[lo+hole]=values[lo+child];hole=child
            if hole==(n-1)//2 and n%2==0:
                values[lo+hole]=values[lo+n-1];hole=n-1
            while hole>top:
                parent=(hole-1)//2
                if values[lo+parent][0]>=v[0]:break
                values[lo+hole]=values[lo+parent];hole=parent
            values[lo+hole]=v
        n=hi-lo
        for hole in range(n//2-1,-1,-1):hole_down(hole,n,values[lo+hole])
        for end in range(n-1,0,-1):
            v=values[lo+end];values[lo+end]=values[lo];hole_down(0,end,v)
    def run(lo,hi,budget):
        while hi-lo>32:
            if budget<=0:heap(lo,hi);return
            left,right=partition(lo,hi);budget=(budget>>1)+(budget>>2)
            if left-lo<hi-right:
                run(lo,left,budget);lo=right
            else:
                run(right,hi,budget);hi=left
        insertion(lo,hi)
    run(0,len(values),ideal)
    return b''.join(v[1] for v in values),stats
