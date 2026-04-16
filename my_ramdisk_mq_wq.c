#include <linux/module.h>
#include <linux/blkdev.h>
#include <linux/workqueue.h>
#include <linux/vmalloc.h>
#include <linux/blk-mq.h>

#include <linux/fs.h> // pour le kernel_write et read
#include <linux/types.h> // pour le ssize_t 


//on va dire que mdr veut My Ram Disk
struct mrd_device {
  //nombre de secteur de 512 octets (genre la taille du disque en secteur)
	sector_t                capacity;
  //truc de cours major mineur pour les disques... faire le lien avec le noyau
	struct gendisk		*mrd_disk;
  // pour gerer la file d'attente ca
	struct blk_mq_tag_set   tag_set;
  // les donnees...
	// void                    *data;
	struct file * fichier_tmp;
};

//cette fonction copier vers my ram disk device, elle va prendre le device vers le quelle elle va copier
// et les structure de donnee bio_vec est juste les octect a copier mais en compliquer a dire jsp pourquoi ils font ca
// et aussi un offset j'imagine que c'est pour savoir ou est ce que on commence a ecrire dans le ficher 
// elle commece par nous dire ou est ce que elle se position dans le ficher et puis elle commence a les octet depuis cette endroit 
// sur notre ficher, mdr->data a la base c'etait la ram mais la c'est notre
//  'disque virtuel', plutot l'adresse du debut des donnes dans le ficher
static int copy_to_mrd(struct mrd_device *mrd, struct bio_vec *bvec,
			loff_t offset)
{
	int retour ;
	void * temp_buff = kmalloc(bvec->bv_len, GFP_KERNEL);
	if (!temp_buff) {
		pr_err("erreur allocation kmalloc copy_to_mdr\n");
		return -ENOMEM;
	}

	pr_info("my_ramdisk: copy_to_mrd (offset = %llu)\n", offset);
	
	// faut retirer cette ligne en bas memcpy... car on a changer en haut data avec ficher_tmp de type struct ficher
	// et on va utiliser la fonction du prof 
	// memcpy_from_bvec(mrd->data + offset, bvec);

	memcpy_from_bvec(temp_buff, bvec); //voir si on test des retours sur ca
	// faire le chiffrement ici non vant de mettre dans buff?
	// le chiffrement c'est ici pour pas modier le bvec directement c'est un ptr
	
	retour = kernel_write(mrd->fichier_tmp, temp_buff,bvec->bv_len, &offset); //caster en int ?
	if (retour < 0){
		//ya une erreur
		pr_err("Erreur lors du write sur le ficher\n");
		kfree(temp_buff);
		return retour;
	}

	kfree(temp_buff);
	return 0;
}

// un peu la meme explication qu'en haut mais en gros c'est pour copier depuis le ficher vers le buffer (le bio vecteur la)
static int copy_from_mrd(struct bio_vec *bvec, struct mrd_device *mrd, loff_t offset)
{
    ssize_t retour;

    void *temp_buff = kmalloc(bvec->bv_len, GFP_KERNEL);
    if (!temp_buff) {
        pr_err("Erreur allocation kmalloc copy_from_mrd\n");
        return -ENOMEM;
    }

    pr_info("my_ramdisk: copy_from_mrd (offset = %llu)\n", offset);

    retour = kernel_read(mrd->fichier_temp, temp_buff, bvec->bv_len, &offset);
    if (retour < 0) {
        pr_err("Erreur lors du read sur le fichier\n");
        kfree(temp_buff);
        return retour;
    }
	//faire le dechiffrement ici non ?avant de mettre dans bvec (pour l'utilisateur) ?
    memcpy_to_bvec(bvec, temp_buff);
    kfree(temp_buff);
    return 0;
}

// c'est ce qui permet au disk de savoir ce qu'il doit faire R ou W 
static int mrd_do_bvec(struct mrd_device *mrd, struct bio_vec *bvec,
		enum req_op op, loff_t offset)
{
	int err = 0;
	switch (op) {
	case REQ_OP_READ:
		err = copy_from_mrd(bvec, mrd, offset);
		break;
	case REQ_OP_WRITE:
		err = copy_to_mrd(mrd, bvec, offset);
		break;
	default:
		err = -EINVAL;
	}

	return err;
}

// my ram disk request worker
// cette structure c'est quand on demande un truc a faire au module de lire (request)
// le ficher j'imagine c'est mdr et le worker s'en occupe quand le kernel le veut
struct mrd_rq_worker {
	// strcut standard de linux pour les workers
	struct work_struct work; 
	// pointeur pour dire sur quel disk on travail
	struct mrd_device *mrd;
	// lq request en question lire ecrire 
	struct request *rq;
};

static void mrd_rq_worker_workfn(struct work_struct *work)
{
	struct mrd_rq_worker *worker = container_of(work, struct mrd_rq_worker, work);
	struct request *rq = worker->rq;

	blk_status_t err = BLK_STS_OK;

	struct bio_vec bvec;
	struct req_iterator iter;

	loff_t pos = blk_rq_pos(rq) << SECTOR_SHIFT;

	struct mrd_device *mrd = worker->mrd;

	loff_t data_len = mrd->capacity << SECTOR_SHIFT;
	unsigned int nr_bytes = 0;

	blk_mq_start_request(rq);

	rq_for_each_segment(bvec, rq, iter) {
		unsigned int len = bvec.bv_len;
		int err_do_bvec;

		if (pos + len > data_len) {
			err = BLK_STS_IOERR;
			break;
		}

		err_do_bvec = mrd_do_bvec(mrd, &bvec, req_op(rq), pos);

		if (err_do_bvec) {
			err = BLK_STS_IOERR;
			goto end_request;
		}
		pos += len;
		nr_bytes += len;
	}

end_request:
	blk_mq_end_request(rq, err);
	module_put(THIS_MODULE);
	kfree(worker);
}

static struct workqueue_struct *workqueue;

static blk_status_t mrd_queue_rq(struct blk_mq_hw_ctx *hctx,
				const struct blk_mq_queue_data *bd)
{
	struct mrd_rq_worker *worker;

	if (!try_module_get(THIS_MODULE)) {
		pr_err("my_ramdisk: unable to get module");
		return BLK_STS_IOERR;
	}

	worker = kzalloc(sizeof *worker, GFP_KERNEL);
	if (!worker) {
		pr_err("my_ramdisk: cannot allocate worker\n");
		module_put(THIS_MODULE);
		return BLK_STS_IOERR;
	}
	worker->mrd = hctx->queue->queuedata;
	worker->rq = bd->rq;
	INIT_WORK(&worker->work, mrd_rq_worker_workfn);
	queue_work(workqueue, &worker->work);

	return BLK_STS_OK;
}

static const struct blk_mq_ops mrd_mq_ops = {
	.queue_rq = mrd_queue_rq,
};

static const struct block_device_operations mrd_fops = {
	.owner =		THIS_MODULE,
};

/*
 * And now the modules code and kernel interface.
 */
static unsigned long mrd_size = 50 * 1024;
module_param(mrd_size, ulong, 0444);
MODULE_PARM_DESC(mrd_size, "Size of each ram disk in kbytes.");

static int max_part = 16;
module_param(max_part, int, 0444);
MODULE_PARM_DESC(max_part, "Num Minors for each devices");

static unsigned int major;

MODULE_LICENSE("GPL");
MODULE_ALIAS("my_ramdisk");

static struct mrd_device *mrd;

static int mrd_alloc(void)
{
	struct gendisk *disk;
	int err = 0;
	char buf[DISK_NAME_LEN];

	mrd = kzalloc(sizeof(*mrd), GFP_KERNEL);
	if (!mrd) {
		err= -ENOMEM;
		goto out;
	}

	snprintf(buf, DISK_NAME_LEN, "my_ram");

	// la a la base c'est un allocation nous on dire comme le TP d'ouvrir juste le fichier
	mrd->data = vmalloc(mrd_size * 1024);
	if (!mrd->data) {
		pr_err("my_ramdisk: not enough memory for ramdisk\n");
		err = -ENOMEM;
		goto out_free_dev;
	}

	// mdr->ficher_temp = 

	mrd->capacity = mrd_size * 2;
	memset(&mrd->tag_set, 0, sizeof(mrd->tag_set));
	mrd->tag_set.ops = &mrd_mq_ops;
	mrd->tag_set.queue_depth = 128;
	mrd->tag_set.numa_node = NUMA_NO_NODE;
//	mrd->tag_set.flags = BLK_MQ_F_SHOULD_MERGE; // Non défini depuis 6.14
	mrd->tag_set.cmd_size = 0;
	mrd->tag_set.driver_data = mrd;
	mrd->tag_set.nr_hw_queues = 1;
	err = blk_mq_alloc_tag_set(&mrd->tag_set);
	if (err) {
		goto out_free_data;
	}

	struct queue_limits lim = {
		.physical_block_size	= PAGE_SIZE,
	};

	disk = mrd->mrd_disk = blk_mq_alloc_disk(&mrd->tag_set, &lim, mrd);
	if (IS_ERR(disk)) {
		err = PTR_ERR(disk);
		pr_err("my_ramdisk: error allocating disk\n");
		goto out_free_tagset;
	}

	disk->major = major;
	disk->first_minor	= 0;
	disk->minors		= max_part;
	disk->fops		= &mrd_fops;
	disk->private_data	= mrd;
	strscpy(disk->disk_name, buf, DISK_NAME_LEN);
	set_capacity(disk, mrd->capacity);

	//blk_queue_physical_block_size(disk->queue, PAGE_SIZE);

	/* Tell the block layer that this is not a rotational device */
	//blk_queue_flag_set(QUEUE_FLAG_NONROT, disk->queue);
	//blk_queue_flag_clear(QUEUE_FLAG_ADD_RANDOM, disk->queue);
	err = add_disk(disk);
	if (err)
		goto out_cleanup_disk;

	pr_info("my_ramdisk: new disk, major = %d, first_minor = %d, minors = %d\n",
		disk->major, disk->first_minor, disk->minors);

	return 0;

out_cleanup_disk:
	put_disk(disk);
out_free_tagset:
	blk_mq_free_tag_set(&mrd->tag_set);
out_free_data:
	vfree(mrd->data);
out_free_dev:
	kfree(mrd);
out:
	return err;
}

static void mrd_cleanup(void)
{
	del_gendisk(mrd->mrd_disk);
	put_disk(mrd->mrd_disk);
	vfree(mrd->data);
	kfree(mrd);
}

static int __init mrd_init(void)
{
	int err;

	if ((major = register_blkdev(0, "my_ramdisk")) < 0) {
		err = -EIO;
		goto out;
	}

	workqueue = alloc_workqueue("my_ramdisk", WQ_MEM_RECLAIM, 1);
	if (!workqueue) {
		err = -ENOMEM;
		goto out_workqueue;
	}

	err = mrd_alloc();
	if (err) {
		goto out_free;
	}

	pr_info("my_ramdisk: module loaded\n");
	return 0;

out_free:
	destroy_workqueue(workqueue);
out_workqueue:
	unregister_blkdev(major, "my_ramdisk");
out:
	pr_info("my_ramdisk: module NOT loaded !!!\n");
	return err;
}

static void __exit mrd_exit(void)
{
	unregister_blkdev(major, "my_ramdisk");
	mrd_cleanup();
	destroy_workqueue(workqueue);

	pr_info("my_ramdisk: module unloaded\n");
}

module_init(mrd_init);
module_exit(mrd_exit);

