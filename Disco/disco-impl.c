/* Necessary includes for device drivers */
#include <linux/init.h>
/* #include <linux/config.h> */
#include <linux/module.h>
#include <linux/kernel.h> /* printk() */
#include <linux/slab.h> /* kmalloc() */
#include <linux/fs.h> /* everything... */
#include <linux/errno.h> /* error codes */
#include <linux/types.h> /* size_t */
#include <linux/proc_fs.h>
#include <linux/fcntl.h> /* O_ACCMODE */
#include <linux/uaccess.h> /* copy_from/to_user */

#include "kmutex.h"

MODULE_LICENSE("Dual BSD/GPL");

//... etc.: copie el resto de ../Syncread/syncread-impl.c ...

/* Declaration of disco.c functions */
int disco_open(struct inode *inode, struct file *filp);
int disco_release(struct inode *inode, struct file *filp);
ssize_t disco_read(struct file *filp, char *buf, size_t count, loff_t *f_pos);
ssize_t disco_write(struct file *filp, const char *buf, size_t count, loff_t *f_pos);
void disco_exit(void);
int disco_init(void);


/* Structure that declares the usual file */
/* access functions */
struct file_operations disco_fops = {
	read: disco_read,
	write: disco_write,
	open: disco_open,
	release: disco_release
};

/* Declaration of the init and exit functions */
module_init(disco_init);
module_exit(disco_exit);



/*** El driver para lecturas sincronas *************************************/

#define TRUE 1
#define FALSE 0

/* Global variables of the driver */

int disco_major = 61;     /* Major number */

/*flags*/
int flagPipeCreado;
int flagPipeLlevado;
ElPipe *pipeUltimoCreado;

/* Buffer to store data */
#define MAX_SIZE 10

static KMutex mutex;
static KCondition lectorEsperaPipe;
static KCondition escritoresEsperanSeTomePipe;
static KCondition escritorEsperaLector;


/*escritor es el que crea pipes */

typedef struct {
	char *buffer;

	ssize_t size;
	int writing;
	int pend_open_write;

	/* El mutex y la condicion para pipe */
	KMutex mutex;
	KCondition cond;
} ElPipe;


ElPipe *pipe_init(void) {
	ElPipe *pipe =kmalloc(sizeof(pipe), GFP_KERNEL);
	pipe->writing= FALSE;
	pipe->pend_open_write= 0;
	pipe->size= 0;
	m_init(&pipe->mutex);
	c_init(&pipe->cond);
	/* Allocating pipe_buffer */
	pipe->buffer = kmalloc(MAX_SIZE, GFP_KERNEL);

	memset(pipe->buffer, 0, MAX_SIZE); /*hasta Max_size los deja en cero*/

	return pipe;
}


int syncread_init(void) {
  int rc;

  /* Registering device */
  rc = register_chrdev(syncread_major, "syncread", &syncread_fops);
  if (rc < 0) {
    printk(
      "<1>syncread: cannot obtain major number %d\n", syncread_major);
    return rc;
  }

  m_init(&mutex);
  c_init(&lectorEsperaPipe);
  c_init(&escritoresEsperanSeTomePipe);
  flagPipeCreado=FALSE;

  printk("<1>Inserting syncread module\n");
  return 0;
}

void syncread_exit(void) { 
  /* Freeing the major number */
  unregister_chrdev(syncread_major, "syncread");

  printk("<1>Removing syncread module\n");
}

int syncread_open(struct inode *inode, struct file *filp) {
	
  int rc= 0;
  m_lock(&mutex);

  if (filp->f_mode & FMODE_WRITE) {
  	while (flagPipeCreado==TRUE){
  		/*espera que el pipe creado se tome por un lector para poder crear otro*/
  		c_wait(&escritoresEsperanSeTomePipe, &mutex);
  	}
  	/*creo un pipe*/
  	ElPipe *p = pipe_init();
  	flagPipeCreado=TRUE;
  	flagPipeLlevado=FALSE;
  	filp->private_data=p;
  	pipeUltimoCreado=p;
  	writing=TRUE;
  	 /*aviso a los lectores que hay un nuevo pipe creado*/
  	c_broadcast(&lectorEsperaPipe);
  	/*m_unlock(&mutex);*/

  	/*esperar que un lector tome el pipe*/
  	/*m_lock(&mutex);*/
  	while(flagPipeLlevado=FALSE){
  		c_wait(&escritorEsperaLector, &mutex);
  	}
  	flagPipeCreado=FALSE;
  	c_broadcast(&escritoresEsperanSeTomePipe);
  	m_unlock(&mutex);
  }
  else if (filp->f_mode & FMODE_READ) {
  	while(flagPipeCreado=FALSE){
  		c_wait(&lectorEsperaPipe, &mutex);
  	}
  	filp->private_data=pipeUltimoCreado;
  	flagPipeLlevado=TRUE;
  	pipeUltimoCreado=NULL;
  	c_broadcast(&escritorEsperaLector);
  	m_unlock(&mutex);
  }

epilog:
  m_unlock(&mutex);
  return rc;
}

int syncread_release(struct inode *inode, struct file *filp) {
	/* liberar memoria del buffer del pipe y luego del pipe*/
	/*solo uno puede hacer free*/
	ElPipe *p = filp->private_data;
	/*esos mutex son del pipe, no globales*/
	m_lock(&p->mutex);
	if (filp->f_mode & FMODE_WRITE) {
		writing= FALSE;
		c_broadcast(&p->cond);
		printk("<1>close for write successful\n");
		m_unlock(&p->mutex);
		kfree(p->buffer);
		kfree(p);
	}
	else if (filp->f_mode & FMODE_READ) {
		c_broadcast(&p->cond);
		printk("<1>close for read (readers remaining=%d)\n", readers);
		m_unlock(&p->mutex);
	}
	else{
		m_unlock(&p->mutex);
		kfree(p->buffer);
		kfree(p);
	}
	return 0;
}

ssize_t syncread_read(struct file *filp, char *buf, size_t count, loff_t *f_pos) {
	ElPipe *p = filp->private_data;

  ssize_t rc;
  m_lock(&p->mutex);

  while (p->size <= *f_pos && p->writing) {
    /* si el lector esta en el final del archivo pero hay un proceso
     * escribiendo todavia en el archivo, el lector espera.
     */
    if (c_wait(&p->cond, &p->mutex)) {
      printk("<1>read interrupted\n");
      rc= -EINTR;
      goto epilog;
    }
  }

  if (count > p->size-*f_pos) {
    count= p->size-*f_pos;
  }

  printk("<1>read %d bytes at %d\n", (int)count, (int)*f_pos);

  /* Transfiriendo datos hacia el espacio del usuario */
  if (copy_to_user(buf, p->buffer+*f_pos, count)!=0) {
    /* el valor de buf es una direccion invalida */
    rc= -EFAULT;
    goto epilog;
  }

  *f_pos+= count;
  rc= count;

epilog:
  m_unlock(&p->mutex);
  return rc;
}

ssize_t syncread_write( struct file *filp, const char *buf, size_t count, loff_t *f_pos) {
	ElPipe *p = filp->private_data;
  ssize_t rc;
  loff_t last;

  m_lock(&p->mutex);

  last= *f_pos + count;
  if (last>MAX_SIZE) {
    count -= last-MAX_SIZE;
  }
  printk("<1>write %d bytes at %d\n", (int)count, (int)*f_pos);

  /* Transfiriendo datos desde el espacio del usuario */
  if (copy_from_user(p->buffer+*f_pos, buf, count)!=0) {
    /* el valor de buf es una direccion invalida */
    rc= -EFAULT;
    goto epilog;
  }

  *f_pos += count;
  p->size= *f_pos;
  rc= count;
  c_broadcast(&p->cond);

epilog:
  m_unlock(&p->mutex);
  return rc;
}

